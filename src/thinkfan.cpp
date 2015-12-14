/********************************************************************
 * thinkfan.cpp: Main program.
 * (C) 2015, Victor Mataré
 *
 * this file is part of thinkfan. See thinkfan.c for further information.
 *
 * thinkfan is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * thinkfan is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with thinkfan.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ******************************************************************/

#include <getopt.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/time.h>
#include <sys/resource.h>

#include <csignal>
#include <cstring>
#include <string>
#include <iostream>
#include <memory>
#include <thread>

#include <fstream>

#include "thinkfan.h"
#include "config.h"
#include "message.h"
#include "error.h"


namespace thinkfan {

bool chk_sanity(true);
bool resume_is_safe(false);
bool quiet(false);
seconds sleeptime(5);
seconds tmp_sleeptime = sleeptime;
float bias_level(5);
int opt;
float depulse = 0;
std::string config_file = CONFIG_DEFAULT;


volatile int interrupted(0);

#ifdef USE_ATASMART
/** Do Not Disturb disk, i.e. don't get temperature from a sleeping disk */
bool dnd_disk = false;
#endif /* USE_ATASMART */


struct TemperatureState *temp_state = nullptr, *last_temp_state = nullptr;


string report_tstat() {
	string rv = "Current temperatures: ";
	for (int temp : temp_state->temps) {
		rv += std::to_string(temp) + ", ";
	}
	return rv.substr(0, rv.length()-2);
}


void sig_handler(int signum) {
	switch(signum) {
	case SIGHUP:
		interrupted = signum;
		break;
	case SIGINT:
	case SIGTERM:
		interrupted = signum;
		break;
	case SIGUSR1:
		log(TF_INF, TF_INF) << report_tstat() << flush;
		break;
	case SIGSEGV:
		// Let's hope memory isn't too fucked up to get through with this ;)
		throw Bug("Segmentation fault.");
		break;
	}
}



void run(const Config &config)
{
	temp_state = new TemperatureState(config.num_temps());
	last_temp_state = new TemperatureState(config.num_temps());

	// Exception-safe pointer deletion
	std::unique_ptr<TemperatureState> deleter1(temp_state);
	std::unique_ptr<TemperatureState> deleter2(last_temp_state);

	tmp_sleeptime = sleeptime;

	// Initially update both last_temp_state and temp_state with the same data
	last_temp_state->temp_it = temp_state->temp_it;
	for (const SensorDriver *sensor : config.sensors()) sensor->read_temps();

	// Then switch back last_temp_state's own copy of the data
	last_temp_state->temp_it = last_temp_state->temps.begin();

	// Set initial fan level
	std::vector<const Level *>::const_iterator cur_lvl = config.levels().begin();
	config.fan()->init();
	while (cur_lvl != config.levels().end() && **cur_lvl <= *temp_state)
		cur_lvl++;
	log(TF_DBG, TF_DBG) << MSG_T_STAT(
			tmp_sleeptime.count(),
			temp_state->tmax,
			last_temp_state->tmax, *temp_state->b_tmax,
			(*cur_lvl)->str()) << flush;
	config.fan()->set_speed(*cur_lvl);

	while (likely(!interrupted)) {
		std::swap(temp_state, last_temp_state);

		temp_state->temp_it = temp_state->temps.begin();
		last_temp_state->temp_it = last_temp_state->temps.begin();
		temp_state->bias = last_temp_state->bias;

		temp_state->tmax = -128;

		for (const SensorDriver *sensor : config.sensors())
			sensor->read_temps();

		if (unlikely(temp_state->temp_it != temp_state->temps.end()))
			fail(TF_ERR) << SystemError(MSG_SENSOR_LOST) << flush;

		if (unlikely(**cur_lvl <= *temp_state)) {
			while (cur_lvl != config.levels().end() && **cur_lvl <= *temp_state)
				cur_lvl++;
			log(TF_DBG, TF_DBG) << MSG_T_STAT(tmp_sleeptime.count(), temp_state->tmax,
					last_temp_state->tmax, *temp_state->b_tmax,
					(*cur_lvl)->str()) << flush;
			config.fan()->set_speed(*cur_lvl);
		}
		else if (unlikely(**cur_lvl > *temp_state)) {
			while (cur_lvl != config.levels().begin() && **cur_lvl > *temp_state)
				cur_lvl--;
			log(TF_DBG, TF_DBG) << MSG_T_STAT(tmp_sleeptime.count(), temp_state->tmax,
					last_temp_state->tmax, *temp_state->b_tmax,
					(*cur_lvl)->str()) << flush;
			config.fan()->set_speed(*cur_lvl);
			tmp_sleeptime = sleeptime;
		}
		else config.fan()->ping_watchdog_and_depulse(*cur_lvl);

		std::this_thread::sleep_for(sleeptime);

		// slowly return bias to 0
		if (unlikely(temp_state->bias != 0)) {
			if (likely(temp_state->bias > 0)) {
				if (temp_state->bias < 0.5) temp_state->bias = 0;
				else temp_state->bias -= temp_state->bias/2 * bias_level;
			}
			else {
				if (temp_state->bias > -0.5) temp_state->bias = 0;
				else temp_state->bias += temp_state->bias/2 * bias_level;
			}
		}
	}
}


int set_options(int argc, char **argv)
{
	const char *optstring = "c:s:b:p::hqDz"
#ifdef USE_ATASMART
			"d";
#else
	;
#endif
	while ((opt = getopt(argc, argv, optstring)) != -1) {
		switch(opt) {
		case 'h':
			std::cerr << MSG_USAGE << std::endl;
			return 1;
			break;
#ifdef USE_ATASMART
		case 'd':
			dnd_disk = true;
			break;
#endif
		case 'c':
			config_file = optarg;
			break;
		case 'q':
			quiet = true;
			break;
		case 'D':
			chk_sanity = false;
			break;
		case 'z':
			resume_is_safe = true;
			break;
		case 's':
			if (optarg) {
				try {
					size_t invalid;
					int s;
					string arg(optarg);
					s = std::stoul(arg, &invalid);
					if (invalid < arg.length())
						fail(TF_ERR) << InvocationError(MSG_OPT_S_INVAL(optarg)) << flush;
					if (s > 15)
						fail(TF_WRN) << InvocationError(MSG_OPT_S_15(s)) << flush;
					else if (s < 0)
						fail(TF_ERR) << InvocationError("Negative sleep time? Seriously?") << flush;
					else if (s < 1)
						fail(TF_WRN) << InvocationError(MSG_OPT_S_1(s)) << flush;
					sleeptime = seconds(static_cast<unsigned int>(s));
				} catch (std::invalid_argument &e) {
					fail(TF_ERR) << InvocationError(MSG_OPT_S_INVAL(optarg)) << flush;
				} catch (std::out_of_range &e) {
					fail(TF_ERR) << InvocationError(MSG_OPT_S_INVAL(optarg)) << flush;
				}
			}
			else fail(TF_ERR) << InvocationError(MSG_OPT_S) << flush;
			break;
		case 'b':
			if (optarg) {
				try {
					size_t invalid;
					float b;
					string arg(optarg);
					b = std::stof(arg, &invalid);
					if (invalid < arg.length())
						fail(TF_WRN) << InvocationError(MSG_OPT_B_INVAL(optarg)) << flush;
					if (b < -10 || b > 30) {
						fail(TF_WRN) << InvocationError(MSG_OPT_B) << flush;
					}
					bias_level = b / 10;
				} catch (std::invalid_argument &e) {
					fail(TF_ERR) << InvocationError(MSG_OPT_B_INVAL(optarg)) << flush;
				} catch (std::out_of_range &e) {
					fail(TF_ERR) << InvocationError(MSG_OPT_B_INVAL(optarg)) << flush;
				}
			}
			else fail(TF_ERR) << InvocationError(MSG_OPT_B_NOARG) << flush;
			break;
		case 'p':
			if (optarg) {
				size_t invalid;
				depulse = std::stof(optarg, &invalid);
				if (invalid != 0 || depulse > 10 || depulse < 0) {
					fail(depulse < 0 ? TF_ERR : TF_WRN) << InvocationError(MSG_OPT_P(optarg)) << flush;
				}
			}
			else depulse = 0.5f;
			break;
		default:
			std::cerr << "Invalid commandline option." << std::endl;
			std::cerr << MSG_USAGE << std::endl;
			return 3;
		}
	}
	if (depulse > 0)
		log(TF_INF, TF_INF) << MSG_DEPULSE(depulse, sleeptime.count()) << flush;

	return 0;
}

}


int main(int argc, char **argv) {
	using namespace thinkfan;


	if (!isatty(fileno(stdout))) Logger::instance().enable_syslog();

	struct sigaction handler;
	memset(&handler, 0, sizeof(handler));
	handler.sa_handler = sig_handler;

	// Install signal handler only after FanControl object has been created
	// since it is used by the handler.
	if (sigaction(SIGHUP, &handler, NULL)
	 || sigaction(SIGINT, &handler, NULL)
	 || sigaction(SIGTERM, &handler, NULL)
	 || sigaction(SIGUSR1, &handler, NULL)
	 || sigaction(SIGSEGV, &handler, NULL)) {
		string msg = strerror(errno);
		log(TF_ERR, TF_ERR) << "sigaction: " << msg;
		return 1;
	}

	std::set_terminate(handle_uncaught);

	try {
		switch (set_options(argc, argv)) {
		case 1:
			return 0;
			break;
		case 0:
			break;
		default:
			return 3;
		}
		std::unique_ptr<const Config> config(Config::read_config(config_file));
		do {
			run(*config);
			if (interrupted == SIGHUP) {
				log(TF_INF, TF_INF) << MSG_RELOAD_CONF << flush;
				try {
					std::unique_ptr<const Config> config_new(Config::read_config(config_file));
					config.swap(config_new);
				} catch(ExpectedError &e) {
					log(TF_ERR, TF_ERR) << MSG_CONF_RELOAD_ERR << flush;
				} catch(std::exception &e) {
					log(TF_ERR, TF_ERR) << "read_config: " << e.what() << flush;
					log(TF_ERR, TF_ERR) << MSG_CONF_RELOAD_ERR << flush;
				}
				interrupted = 0;
			}
		} while (!interrupted);

		log(TF_INF, TF_INF) << MSG_TERM << flush;
	}
	catch (ExpectedError &e) {
		log(TF_DBG, TF_DBG) << "Backtrace:" << flush << e.backtrace() << flush;
		return 1;
	}
	catch (Bug &e) {
		fail(TF_ERR) << e.what() << flush <<
				"Backtrace:" << flush <<
				e.backtrace() << flush <<
				MSG_BUG << flush;
		return 2;
	}

	return 0;
}

