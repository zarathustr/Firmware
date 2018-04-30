/****************************************************************************
 *
 *   Copyright (c) 2012-2018, 2017 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "RCInput.hpp"

#define SCHEDULE_INTERVAL	4000	/**< The schedule interval in usec (250 Hz) */

#ifdef SPEKTRUM_POWER
static bool dsm_bind_start(int arg);
#endif /* SPEKTRUM_POWER */

work_s RCInput::_work = {};

RCInput::RCInput()
{
	// rc input, published to ORB
	_rc_in.input_source = input_rc_s::RC_INPUT_SOURCE_PX4FMU_PPM;

	// initialize it as RC lost
	_rc_in.rc_lost = true;

	// initialize raw_rc values and count
	for (unsigned i = 0; i < input_rc_s::RC_INPUT_MAX_CHANNELS; i++) {
		_raw_rc_values[i] = UINT16_MAX;
	}

#ifdef GPIO_SBUS_INV
	// this board has a GPIO to control SBUS inversion
	px4_arch_configgpio(GPIO_SBUS_INV);
#endif
}

RCInput::~RCInput()
{
	orb_unadvertise(_to_input_rc);

#ifdef RC_SERIAL_PORT
	dsm_deinit();
#endif
}

int
RCInput::init()
{
	_adc_sub = orb_subscribe(ORB_ID(adc_report));

#ifdef RC_SERIAL_PORT

#  ifdef RF_RADIO_POWER_CONTROL
	// power radio on
	RF_RADIO_POWER_CONTROL(true);
#  endif
	_vehicle_cmd_sub = orb_subscribe(ORB_ID(vehicle_command));
	// dsm_init sets some file static variables and returns a file descriptor
	_rcs_fd = dsm_init(RC_SERIAL_PORT);
	// assume SBUS input
	sbus_config(_rcs_fd, false);
#  ifdef GPIO_PPM_IN
	// disable CPPM input by mapping it away from the timer capture input
	px4_arch_unconfiggpio(GPIO_PPM_IN);
#  endif
#endif

	return 0;
}

int
RCInput::task_spawn(int argc, char *argv[])
{
	/* schedule a cycle to start things */
	int ret = work_queue(HPWORK, &_work, (worker_t)&RCInput::cycle_trampoline, nullptr, 0);

	if (ret < 0) {
		return ret;
	}

	_task_id = task_id_is_work_queue;

	// wait until task is up & running (the mode_* commands depend on it)
	if (wait_until_running() < 0) {
		_task_id = -1;
		return -1;
	}

	return PX4_OK;
}

void
RCInput::cycle_trampoline(void *arg)
{
	RCInput *dev = reinterpret_cast<RCInput *>(arg);

	// check if the trampoline is called for the first time
	if (!dev) {
		dev = new RCInput();

		if (!dev) {
			PX4_ERR("alloc failed");
			return;
		}

		if (dev->init() != 0) {
			PX4_ERR("init failed");
			delete dev;
			return;
		}

		_object = dev;
	}

	dev->cycle();
}

void
RCInput::fill_rc_in(uint16_t raw_rc_count_local,
		    uint16_t raw_rc_values_local[input_rc_s::RC_INPUT_MAX_CHANNELS],
		    hrt_abstime now, bool frame_drop, bool failsafe,
		    unsigned frame_drops, int rssi = -1)
{
	// fill rc_in struct for publishing
	_rc_in.channel_count = raw_rc_count_local;

	if (_rc_in.channel_count > input_rc_s::RC_INPUT_MAX_CHANNELS) {
		_rc_in.channel_count = input_rc_s::RC_INPUT_MAX_CHANNELS;
	}

	unsigned valid_chans = 0;

	for (unsigned i = 0; i < _rc_in.channel_count; i++) {
		_rc_in.values[i] = raw_rc_values_local[i];

		if (raw_rc_values_local[i] != UINT16_MAX) {
			valid_chans++;
		}

		// once filled, reset values back to default
		_raw_rc_values[i] = UINT16_MAX;
	}

	_rc_in.timestamp = now;
	_rc_in.timestamp_last_signal = _rc_in.timestamp;
	_rc_in.rc_ppm_frame_length = 0;

	/* fake rssi if no value was provided */
	if (rssi == -1) {

		/* set RSSI if analog RSSI input is present */
		if (_analog_rc_rssi_stable) {
			float rssi_analog = ((_analog_rc_rssi_volt - 0.2f) / 3.0f) * 100.0f;

			if (rssi_analog > 100.0f) {
				rssi_analog = 100.0f;
			}

			if (rssi_analog < 0.0f) {
				rssi_analog = 0.0f;
			}

			_rc_in.rssi = rssi_analog;

		} else {
			_rc_in.rssi = 255;
		}

	} else {
		_rc_in.rssi = rssi;
	}

	if (valid_chans == 0) {
		_rc_in.rssi = 0;
	}

	_rc_in.rc_failsafe = failsafe;
	_rc_in.rc_lost = (valid_chans == 0);
	_rc_in.rc_lost_frame_count = frame_drops;
	_rc_in.rc_total_frame_count = 0;
}

#ifdef RC_SERIAL_PORT
void RCInput::set_rc_scan_state(RC_SCAN newState)
{
//    PX4_WARN("RCscan: %s failed, trying %s", RCInput::RC_SCAN_STRING[_rc_scan_state], RCInput::RC_SCAN_STRING[newState]);
	_rc_scan_begin = 0;
	_rc_scan_state = newState;
}

void RCInput::rc_io_invert(bool invert)
{
	INVERT_RC_INPUT(invert);
}
#endif

void
RCInput::run()
{
	if (init() != 0) {
		PX4_ERR("init failed");
		exit_and_cleanup();
		return;
	}

	cycle();
}

void
RCInput::cycle()
{
	_cycle_timestamp = hrt_absolute_time();

#if defined(RC_SERIAL_PORT) && defined(SPEKTRUM_POWER)
	/* vehicle command */
	bool updated = false;
	orb_check(_vehicle_cmd_sub, &updated);

	if (updated) {
		struct vehicle_command_s cmd;
		orb_copy(ORB_ID(vehicle_command), _vehicle_cmd_sub, &cmd);

		// Check for a pairing command
		if ((unsigned int)cmd.command == vehicle_command_s::VEHICLE_CMD_START_RX_PAIR) {
			if ((int)cmd.param1 == 0) {
				// DSM binding command
				int dsm_bind_mode = (int)cmd.param2;

				int dsm_bind_pulses = 0;

				if (dsm_bind_mode == 0) {
					dsm_bind_pulses = DSM2_BIND_PULSES;

				} else if (dsm_bind_mode == 1) {
					dsm_bind_pulses = DSMX_BIND_PULSES;

				} else {
					dsm_bind_pulses = DSMX8_BIND_PULSES;
				}

				dsm_bind_start(dsm_bind_pulses);
			}
		}
	}

#endif /* RC_SERIAL_PORT && SPEKTRUM_POWER */

	/* update ADC sampling */
#ifdef ADC_RC_RSSI_CHANNEL
	orb_check(_adc_sub, &updated);

	if (updated) {

		struct adc_report_s adc;
		orb_copy(ORB_ID(adc_report), _adc_sub, &adc);
		const unsigned adc_chans = sizeof(adc.channel_id) / sizeof(adc.channel_id[0]);

		for (unsigned i = 0; i < adc_chans; i++) {
			if (adc.channel_id[i] == ADC_RC_RSSI_CHANNEL) {

				if (_analog_rc_rssi_volt < 0.0f) {
					_analog_rc_rssi_volt = adc.channel_value[i];
				}

				_analog_rc_rssi_volt = _analog_rc_rssi_volt * 0.995f + adc.channel_value[i] * 0.005f;

				/* only allow this to be used if we see a high RSSI once */
				if (_analog_rc_rssi_volt > 2.5f) {
					_analog_rc_rssi_stable = true;
				}
			}
		}
	}

#endif

	bool rc_updated = false;

#ifdef RC_SERIAL_PORT
	// This block scans for a supported serial RC input and locks onto the first one found
	// Scan for 300 msec, then switch protocol
	constexpr hrt_abstime rc_scan_max = 300 * 1000;

	bool sbus_failsafe, sbus_frame_drop;
	unsigned frame_drops;
	bool dsm_11_bit;


	if (_report_lock && _rc_scan_locked) {
		_report_lock = false;
		//PX4_WARN("RCscan: %s RC input locked", RC_SCAN_STRING[_rc_scan_state]);
	}

	// read all available data from the serial RC input UART
	int newBytes = ::read(_rcs_fd, &_rcs_buf[0], SBUS_BUFFER_SIZE);

	switch (_rc_scan_state) {
	case RC_SCAN_SBUS:
		if (_rc_scan_begin == 0) {
			_rc_scan_begin = _cycle_timestamp;
			// Configure serial port for SBUS
			sbus_config(_rcs_fd, false);
			rc_io_invert(true);

		} else if (_rc_scan_locked
			   || _cycle_timestamp - _rc_scan_begin < rc_scan_max) {

			// parse new data
			if (newBytes > 0) {
				rc_updated = sbus_parse(_cycle_timestamp, &_rcs_buf[0], newBytes, &_raw_rc_values[0], &_raw_rc_count, &sbus_failsafe,
							&sbus_frame_drop, &frame_drops, input_rc_s::RC_INPUT_MAX_CHANNELS);

				if (rc_updated) {
					// we have a new SBUS frame. Publish it.
					_rc_in.input_source = input_rc_s::RC_INPUT_SOURCE_PX4FMU_SBUS;
					fill_rc_in(_raw_rc_count, _raw_rc_values, _cycle_timestamp, sbus_frame_drop, sbus_failsafe, frame_drops);
					_rc_scan_locked = true;
				}
			}

		} else {
			// Scan the next protocol
			set_rc_scan_state(RC_SCAN_DSM);
		}

		break;

	case RC_SCAN_DSM:
		if (_rc_scan_begin == 0) {
			_rc_scan_begin = _cycle_timestamp;
			//			// Configure serial port for DSM
			dsm_config(_rcs_fd);
			rc_io_invert(false);

		} else if (_rc_scan_locked
			   || _cycle_timestamp - _rc_scan_begin < rc_scan_max) {

			if (newBytes > 0) {
				// parse new data
				rc_updated = dsm_parse(_cycle_timestamp, &_rcs_buf[0], newBytes, &_raw_rc_values[0], &_raw_rc_count,
						       &dsm_11_bit, &frame_drops, input_rc_s::RC_INPUT_MAX_CHANNELS);

				if (rc_updated) {
					// we have a new DSM frame. Publish it.
					_rc_in.input_source = input_rc_s::RC_INPUT_SOURCE_PX4FMU_DSM;
					fill_rc_in(_raw_rc_count, _raw_rc_values, _cycle_timestamp, false, false, frame_drops);
					_rc_scan_locked = true;
				}
			}

		} else {
			// Scan the next protocol
			set_rc_scan_state(RC_SCAN_ST24);
		}

		break;

	case RC_SCAN_ST24:
		if (_rc_scan_begin == 0) {
			_rc_scan_begin = _cycle_timestamp;
			// Configure serial port for DSM
			dsm_config(_rcs_fd);
			rc_io_invert(false);

		} else if (_rc_scan_locked
			   || _cycle_timestamp - _rc_scan_begin < rc_scan_max) {

			if (newBytes > 0) {
				// parse new data
				uint8_t st24_rssi, lost_count;

				rc_updated = false;

				for (unsigned i = 0; i < (unsigned)newBytes; i++) {
					/* set updated flag if one complete packet was parsed */
					st24_rssi = RC_INPUT_RSSI_MAX;
					rc_updated = (OK == st24_decode(_rcs_buf[i], &st24_rssi, &lost_count, &_raw_rc_count, _raw_rc_values,
									input_rc_s::RC_INPUT_MAX_CHANNELS));
				}

				// The st24 will keep outputting RC channels and RSSI even if RC has been lost.
				// The only way to detect RC loss is therefore to look at the lost_count.

				if (rc_updated) {
					if (lost_count == 0) {
						// we have a new ST24 frame. Publish it.
						_rc_in.input_source = input_rc_s::RC_INPUT_SOURCE_PX4FMU_ST24;
						fill_rc_in(_raw_rc_count, _raw_rc_values, _cycle_timestamp, false, false, frame_drops, st24_rssi);
						_rc_scan_locked = true;

					} else {
						// if the lost count > 0 means that there is an RC loss
						_rc_in.rc_lost = true;
					}
				}
			}

		} else {
			// Scan the next protocol
			set_rc_scan_state(RC_SCAN_SUMD);
		}

		break;

	case RC_SCAN_SUMD:
		if (_rc_scan_begin == 0) {
			_rc_scan_begin = _cycle_timestamp;
			// Configure serial port for DSM
			dsm_config(_rcs_fd);
			rc_io_invert(false);

		} else if (_rc_scan_locked
			   || _cycle_timestamp - _rc_scan_begin < rc_scan_max) {

			if (newBytes > 0) {
				// parse new data
				uint8_t sumd_rssi, rx_count;
				bool sumd_failsafe;

				rc_updated = false;

				for (unsigned i = 0; i < (unsigned)newBytes; i++) {
					/* set updated flag if one complete packet was parsed */
					sumd_rssi = RC_INPUT_RSSI_MAX;
					rc_updated = (OK == sumd_decode(_rcs_buf[i], &sumd_rssi, &rx_count, &_raw_rc_count, _raw_rc_values,
									input_rc_s::RC_INPUT_MAX_CHANNELS, &sumd_failsafe));
				}

				if (rc_updated) {
					// we have a new SUMD frame. Publish it.
					_rc_in.input_source = input_rc_s::RC_INPUT_SOURCE_PX4FMU_SUMD;
					fill_rc_in(_raw_rc_count, _raw_rc_values, _cycle_timestamp, false, sumd_failsafe, frame_drops, sumd_rssi);
					_rc_scan_locked = true;
				}
			}

		} else {
			// Scan the next protocol
			set_rc_scan_state(RC_SCAN_PPM);
		}

		break;

	case RC_SCAN_PPM:
		// skip PPM if it's not supported
#ifdef HRT_PPM_CHANNEL
		if (_rc_scan_begin == 0) {
			_rc_scan_begin = _cycle_timestamp;
			// Configure timer input pin for CPPM
			px4_arch_configgpio(GPIO_PPM_IN);
			rc_io_invert(false);

		} else if (_rc_scan_locked || _cycle_timestamp - _rc_scan_begin < rc_scan_max) {

			// see if we have new PPM input data
			if ((ppm_last_valid_decode != _rc_in.timestamp_last_signal) && ppm_decoded_channels > 3) {
				// we have a new PPM frame. Publish it.
				rc_updated = true;
				_rc_in.input_source = input_rc_s::RC_INPUT_SOURCE_PX4FMU_PPM;
				fill_rc_in(ppm_decoded_channels, ppm_buffer, _cycle_timestamp, false, false, 0);
				_rc_scan_locked = true;
				_rc_in.rc_ppm_frame_length = ppm_frame_length;
				_rc_in.timestamp_last_signal = ppm_last_valid_decode;
			}

		} else {
			// disable CPPM input by mapping it away from the timer capture input
			px4_arch_unconfiggpio(GPIO_PPM_IN);
			// Scan the next protocol
			set_rc_scan_state(RC_SCAN_SBUS);
		}

#else   // skip PPM if it's not supported
		set_rc_scan_state(RC_SCAN_SBUS);

#endif  // HRT_PPM_CHANNEL

		break;
	}

#else  // RC_SERIAL_PORT not defined
#ifdef HRT_PPM_CHANNEL

	// see if we have new PPM input data
	if ((ppm_last_valid_decode != _rc_in.timestamp_last_signal) && ppm_decoded_channels > 3) {
		// we have a new PPM frame. Publish it.
		rc_updated = true;
		fill_rc_in(ppm_decoded_channels, ppm_buffer, _cycle_timestamp, false, false, 0);
		_rc_in.rc_ppm_frame_length = ppm_frame_length;
		_rc_in.timestamp_last_signal = ppm_last_valid_decode;
	}

#endif  // HRT_PPM_CHANNEL
#endif  // RC_SERIAL_PORT

	if (rc_updated) {
		/* lazily advertise on first publication */
		if (_to_input_rc == nullptr) {
			int instance;
			_to_input_rc = orb_advertise_multi(ORB_ID(input_rc), &_rc_in, &instance, ORB_PRIO_DEFAULT);

		} else {
			orb_publish(ORB_ID(input_rc), _to_input_rc, &_rc_in);
		}

	} else if (!rc_updated && ((hrt_absolute_time() - _rc_in.timestamp_last_signal) > 1000 * 1000)) {
		_rc_scan_locked = false;
	}

	if (should_exit()) {
		exit_and_cleanup();

	} else {
		/* schedule next cycle */
		work_queue(HPWORK, &_work, (worker_t)&RCInput::cycle_trampoline, this, USEC2TICK(SCHEDULE_INTERVAL));
	}
}

#ifdef SPEKTRUM_POWER
bool
dsm_bind_start(int arg)
{
	int ret = PX4_ERROR;

	/* specify 11ms DSMX. RX will automatically fall back to 22ms or DSM2 if necessary */

	/* only allow DSM2, DSM-X and DSM-X with more than 7 channels */
	PX4_INFO("DSM_BIND_START: DSM%s RX", (arg == 0) ? "2" : ((arg == 1) ? "-X" : "-X8"));

	if (arg == DSM2_BIND_PULSES ||
	    arg == DSMX_BIND_PULSES ||
	    arg == DSMX8_BIND_PULSES) {

		dsm_bind(DSM_CMD_BIND_POWER_DOWN, 0);

		dsm_bind(DSM_CMD_BIND_SET_RX_OUT, 0);
		usleep(500000);

		dsm_bind(DSM_CMD_BIND_POWER_UP, 0);
		usleep(72000);

		irqstate_t flags = px4_enter_critical_section();
		dsm_bind(DSM_CMD_BIND_SEND_PULSES, arg);
		px4_leave_critical_section(flags);
		usleep(50000);

		dsm_bind(DSM_CMD_BIND_REINIT_UART, 0);

		ret = OK;

	} else {
		PX4_ERR("DSM bind failed");
		ret = -EINVAL;
	}

	return (ret == PX4_OK);
}
#endif /* SPEKTRUM_POWER */

RCInput *RCInput::instantiate(int argc, char *argv[])
{
	return new RCInput();
}

int RCInput::custom_command(int argc, char *argv[])
{
#ifdef SPEKTRUM_POWER
	const char *verb = argv[0];

	if (!strcmp(verb, "bind")) {
		dsm_bind_start(DSMX8_BIND_PULSES);
		return 0;
	}

#endif /* SPEKTRUM_POWER */

	/* start the FMU if not running */
	if (!is_running()) {
		int ret = RCInput::task_spawn(argc, argv);

		if (ret) {
			return ret;
		}
	}

	return print_usage("unknown command");
}

int RCInput::print_usage(const char *reason)
{
	if (reason) {
		PX4_WARN("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
This module is responsible for reading the input pins.

It does the RC input parsing and auto-selecting the method. Supported methods are:
- PPM
- SBUS
- DSM
- SUMD
- ST24

)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("rc_input", "driver");
	PRINT_MODULE_USAGE_COMMAND_DESCR("start", "Start the task (without any mode set, use any of the mode_* cmds)");

	PRINT_MODULE_USAGE_COMMAND_DESCR("bind", "Send a DSM bind command (module must be running)");

	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();

	return 0;
}

int RCInput::print_status()
{
	PX4_INFO("RC scan state: %s", RC_SCAN_STRING[_rc_scan_state]);
#ifdef RC_SERIAL_PORT
	PX4_INFO("SBUS frame drops: %u", sbus_dropped_frames());
#endif

	return 0;
}

extern "C" __EXPORT int rc_input_main(int argc, char *argv[]);

int
rc_input_main(int argc, char *argv[])
{
	return RCInput::main(argc, argv);
}