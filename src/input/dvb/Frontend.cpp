/* Frontend.cpp

   Copyright (C) 2015, 2016 Marc Postema (mpostema09 -at- gmail.com)

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
   Or, point your browser to http://www.gnu.org/copyleft/gpl.html
 */
#include <input/dvb/Frontend.h>

#include <Log.h>
#include <Utils.h>
#include <Stream.h>
#include <StringConverter.h>
#include <mpegts/PacketBuffer.h>
#include <input/dvb/FrontendData.h>
#include <input/dvb/delivery/DVBC.h>
#include <input/dvb/delivery/DVBS.h>
#include <input/dvb/delivery/DVBT.h>

#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/dvb/dmx.h>

namespace input {
namespace dvb {

	Frontend::Frontend() :
		_tuned(false),
		_fd_fe(-1),
		_fd_dvr(-1),
		_dvbs2(0),
		_dvbt(0),
		_dvbt2(0),
		_dvbc(0),
		_dvbc2(0),
		_dvrBufferSize(40 * 188 * 1024) {
		snprintf(_fe_info.name, sizeof(_fe_info.name), "Not Set");
		_path_to_fe  = "Not Set";
		_path_to_dvr = "Not Set";
		_path_to_dmx = "Not Set";
	}

	Frontend::~Frontend() {
		for (input::dvb::delivery::SystemVector::iterator it = _deliverySystem.begin();
		     it != _deliverySystem.end();
		     ++it) {
			DELETE(*it);
		}
	}

	// =======================================================================
	//  -- Static functions --------------------------------------------------
	// =======================================================================

	int getAttachedFrontends(StreamVector &stream, decrypt::dvbapi::Client *decrypt,
			const std::string &path, int count) {
#define DMX      "/dev/dvb/adapter%d/demux%d"
#define DVR      "/dev/dvb/adapter%d/dvr%d"
#define FRONTEND "/dev/dvb/adapter%d/frontend%d"

#define FE_PATH_LEN 255
#if SIMU
		UNUSED(path)
		count = 2;
		char fe_path[FE_PATH_LEN];
		char dvr_path[FE_PATH_LEN];
		char dmx_path[FE_PATH_LEN];
		snprintf(fe_path,  FE_PATH_LEN, FRONTEND, 0, 0);
		snprintf(dvr_path, FE_PATH_LEN, DVR, 0, 0);
		snprintf(dmx_path, FE_PATH_LEN, DMX, 0, 0);
		stream.push_back(new Stream(0, decrypt));
		stream[0]->setFrontendInfo(fe_path, dvr_path, dmx_path);
		snprintf(fe_path,  FE_PATH_LEN, FRONTEND, 1, 0);
		snprintf(dvr_path, FE_PATH_LEN, DVR, 1, 0);
		snprintf(dmx_path, FE_PATH_LEN, DMX, 1, 0);
		stream.push_back(new Stream(1, decrypt));
		stream[1]->setFrontendInfo(fe_path, dvr_path, dmx_path);
#else
		struct dirent **file_list;
		const int n = scandir(path.c_str(), &file_list, nullptr, alphasort);
		if (n > 0) {
			int i;
			for (i = 0; i < n; ++i) {
				char full_path[FE_PATH_LEN];
				snprintf(full_path, FE_PATH_LEN, "%s/%s", path.c_str(), file_list[i]->d_name);
				struct stat stat_buf;
				if (stat(full_path, &stat_buf) == 0) {
					switch (stat_buf.st_mode & S_IFMT) {
						case S_IFCHR: // character device
							if (strstr(file_list[i]->d_name, "frontend") != nullptr) {
								int fe_nr;
								sscanf(file_list[i]->d_name, "frontend%d", &fe_nr);
								int adapt_nr;
								sscanf(path.c_str(), "/dev/dvb/adapter%d", &adapt_nr);

								// make new paths
								char fe_path[FE_PATH_LEN];
								char dvr_path[FE_PATH_LEN];
								char dmx_path[FE_PATH_LEN];
								snprintf(fe_path,  FE_PATH_LEN, FRONTEND, adapt_nr, fe_nr);
								snprintf(dvr_path, FE_PATH_LEN, DVR, adapt_nr, fe_nr);
								snprintf(dmx_path, FE_PATH_LEN, DMX, adapt_nr, fe_nr);

								stream.push_back(new Stream(count, decrypt));
								stream[count]->setFrontendInfo(fe_path, dvr_path, dmx_path);
								++count;
							}
							break;
						case S_IFDIR:
							// do not use dir '.' an '..'
							if (strcmp(file_list[i]->d_name, ".") != 0 && strcmp(file_list[i]->d_name, "..") != 0) {
								count = getAttachedFrontends(stream, decrypt, full_path, count);
							}
							break;
					}
				}
				free(file_list[i]);
			}
		}
#endif
#undef DMX
#undef DVR
#undef FRONTEND
#undef FE_PATH_LEN
		return count;
	}

	// =======================================================================
	//  -- Static member functions -------------------------------------------
	// =======================================================================

	void Frontend::enumerate(StreamVector &stream, decrypt::dvbapi::Client *decrypt,
		const std::string &path) {
		SI_LOG_INFO("Detecting frontends in: %s", path.c_str());
		const int count = getAttachedFrontends(stream, decrypt, path, 0);
		SI_LOG_INFO("Frontends found: %zu", count);
	}

	// =======================================================================
	//  -- base::XMLSupport --------------------------------------------------
	// =======================================================================

	void Frontend::addToXML(std::string &xml) const {
		base::MutexLock lock(_mutex);
		StringConverter::addFormattedString(xml, "<frontendname>%s</frontendname>", _fe_info.name);
		StringConverter::addFormattedString(xml, "<pathname>%s</pathname>", _path_to_fe.c_str());
		StringConverter::addFormattedString(xml, "<freq>%d Hz to %d Hz</freq>", _fe_info.frequency_min, _fe_info.frequency_max);
		StringConverter::addFormattedString(xml, "<symbol>%d symbols/s to %d symbols/s</symbol>", _fe_info.symbol_rate_min, _fe_info.symbol_rate_max);

		// Monitor
		StringConverter::addFormattedString(xml, "<status>%d</status>", _status);
		StringConverter::addFormattedString(xml, "<signal>%d</signal>", _strength);
		StringConverter::addFormattedString(xml, "<snr>%d</snr>", _snr);
		StringConverter::addFormattedString(xml, "<ber>%d</ber>", _ber);
		StringConverter::addFormattedString(xml, "<unc>%d</unc>", _ublocks);

		ADD_CONFIG_NUMBER_INPUT(xml, "dvrbuffer", _dvrBufferSize, (10 * 188 * 1024), (80 * 188 * 1024));

		ADD_XML_BEGIN_ELEMENT(xml, "deliverySystem");
		_deliverySystem[0]->addToXML(xml);
		ADD_XML_END_ELEMENT(xml, "deliverySystem");
	}

	void Frontend::fromXML(const std::string &xml) {
		base::MutexLock lock(_mutex);
		std::string element;
		if (findXMLElement(xml, "deliverySystem", element)) {
			_deliverySystem[0]->fromXML(element);
		}
		if (findXMLElement(xml, "dvrbuffer.value", element)) {
			_dvrBufferSize = atoi(element.c_str());
		}
	}

	// =======================================================================
	//  -- input::Device -----------------------------------------------------
	// =======================================================================

	void Frontend::addDeliverySystemCount(
		std::size_t &dvbs2,
		std::size_t &dvbt,
		std::size_t &dvbt2,
		std::size_t &dvbc,
		std::size_t &dvbc2) {
		dvbs2 += _dvbs2;
		dvbt  += _dvbt;
		dvbt2 += _dvbt2;
		dvbc  += _dvbc;
		dvbc2 += _dvbc2;
	}

	bool Frontend::isDataAvailable() {
		struct pollfd pfd[1];
		pfd[0].fd = _fd_dvr;
		pfd[0].events = POLLIN | POLLPRI;
		pfd[0].revents = 0;
		return poll(pfd, 1, 100) > 0;
	}

	bool Frontend::readFullTSPacket(mpegts::PacketBuffer &buffer) {
		// try read maximum amount of bytes from DVR
		const int bytes = read(_fd_dvr, buffer.getWriteBufferPtr(), buffer.getAmountOfBytesToWrite());
		if (bytes > 0) {
/*
			// sync byte then check cc
			if (_bufferPtrWrite[0] == 0x47 && bytes_read > 3) {
				// get PID and CC from TS
				const uint16_t pid = ((_bufferPtrWrite[1] & 0x1f) << 8) | _bufferPtrWrite[2];
				const uint8_t  cc  =   _bufferPtrWrite[3] & 0x0f;
				_stream.addPIDData(pid, cc);
			}
*/
			buffer.addAmountOfBytesWritten(bytes);
			return buffer.full();
		}
		return false;
	}

	bool Frontend::capableOf(fe_delivery_system_t msys) {
		for (input::dvb::delivery::SystemVector::iterator it = _deliverySystem.begin();
		     it != _deliverySystem.end();
		     ++it) {
			if ((*it)->isCapableOf(msys)) {
				return true;
			}
		}
		return false;
	}

	void Frontend::monitorSignal(bool showStatus) {
		base::MutexLock lock(_mutex);
		// first read status
		if (ioctl(_fd_fe, FE_READ_STATUS, &_status) == 0) {
			// some frontends might not support all these ioctls
			if (ioctl(_fd_fe, FE_READ_SIGNAL_STRENGTH, &_strength) != 0) {
				_strength = 0;
			}
			if (ioctl(_fd_fe, FE_READ_SNR, &_snr) != 0) {
				_snr = 0;
			}
			if (ioctl(_fd_fe, FE_READ_BER, &_ber) != 0) {
				_ber = 0;
			}
			if (ioctl(_fd_fe, FE_READ_UNCORRECTED_BLOCKS, &_ublocks) != 0) {
				_ublocks = 0;
			}
			_strength = (_strength * 240) / 0xffff;
			_snr = (_snr * 15) / 0xffff;

			// Print Status
			if (showStatus) {
				SI_LOG_INFO("status %02x | signal %3u%% | snr %3u%% | ber %d | unc %d | Locked %d",
					_status, _strength, _snr, _ber, _ublocks,
					(_status & FE_HAS_LOCK) ? 1 : 0);
			}
		} else {
			PERROR("FE_READ_STATUS failed");
		}
	}

	bool Frontend::update(const int streamID, input::DeviceData *data) {
		SI_LOG_DEBUG("Stream: %d, Updating frontend...", streamID);
		input::dvb::FrontendData *frontendData = dynamic_cast<input::dvb::FrontendData *>(data);
		if (frontendData != nullptr) {
#ifndef SIMU
			// Setup, tune and set PID Filters
			if (frontendData->hasFrontendDataChanged()) {
				frontendData->resetFrontendDataChanged();
				_tuned = false;
				CLOSE_FD(_fd_dvr);
			}

			std::size_t timeout = 0;
			while (!setupAndTune(streamID, *frontendData)) {
				usleep(150000);
				++timeout;
				if (timeout > 3) {
					return false;
				}
			}

			updatePIDFilters(streamID, *frontendData);
#endif
			SI_LOG_DEBUG("Stream: %d, Updating frontend (Finished)", streamID);
		} else {
			SI_LOG_ERROR("Stream: %d, Updating frontend failed because of wrong data", streamID);
			return false;
		}
		return true;
	}

	bool Frontend::teardown(const int streamID, input::DeviceData *data) {
		input::dvb::FrontendData *frontendData = dynamic_cast<input::dvb::FrontendData *>(data);
		if (frontendData != nullptr) {
			for (std::size_t i = 0; i < MAX_PIDS; ++i) {
				if (frontendData->isPIDUsed(i)) {
					SI_LOG_DEBUG("Stream: %d, Remove filter PID: %04d - fd: %03d - Packet Count: %d",
							streamID, i, frontendData->getDMXFileDescriptor(i), frontendData->getPacketCounter(i));
					resetPid(i, *frontendData);
				} else if (frontendData->getDMXFileDescriptor(i) != -1) {
					SI_LOG_ERROR("Stream: %d, !! No PID %d but still open DMX !!", streamID, i);
					resetPid(i, *frontendData);
				}
			}
			_tuned = false;
			CLOSE_FD(_fd_fe);
			CLOSE_FD(_fd_dvr);
		} else {
			SI_LOG_ERROR("Stream: %d, Teardown frontend failed because of wrong data", streamID);
			return false;
		}
		return true;
	}

	bool Frontend::setFrontendInfo(const std::string &fe,
				const std::string &dvr,	const std::string &dmx) {
		_path_to_fe = fe;
		_path_to_dvr = dvr;
		_path_to_dmx = dmx;

#if SIMU
		sprintf(_fe_info.name, "Simulation DVB-S2/C/T Card");
		_fe_info.frequency_min = 1000000UL;
		_fe_info.frequency_max = 21000000UL;
		_fe_info.symbol_rate_min = 20000UL;
		_fe_info.symbol_rate_max = 250000UL;
#else
		int fd_fe;
		// open frontend in readonly mode
		if ((fd_fe = open_fe(_path_to_fe, true)) < 0) {
			snprintf(_fe_info.name, sizeof(_fe_info.name), "Not Found");
			PERROR("open_fe");
			return false;
		}

		if (ioctl(fd_fe, FE_GET_INFO, &_fe_info) != 0) {
			snprintf(_fe_info.name, sizeof(_fe_info.name), "Not Set");
			PERROR("FE_GET_INFO");
			CLOSE_FD(fd_fe);
			return false;
		}
#endif
		SI_LOG_INFO("Frontend Name: %s", _fe_info.name);

		struct dtv_property dtvProperty;
#if SIMU
		dtvProperty.u.buffer.len = 4;
		dtvProperty.u.buffer.data[0] = SYS_DVBS;
		dtvProperty.u.buffer.data[1] = SYS_DVBS2;
		dtvProperty.u.buffer.data[2] = SYS_DVBT;
#  if FULL_DVB_API_VERSION >= 0x0505
		dtvProperty.u.buffer.data[3] = SYS_DVBC_ANNEX_A;
#  else
		dtvProperty.u.buffer.data[3] = SYS_DVBC_ANNEX_AC;
#  endif
#else
		dtvProperty.cmd = DTV_ENUM_DELSYS;
		dtvProperty.u.data = DTV_UNDEFINED;

		struct dtv_properties dtvProperties;
		dtvProperties.num = 1;       // size
		dtvProperties.props = &dtvProperty;
		if (ioctl(fd_fe, FE_GET_PROPERTY, &dtvProperties ) != 0) {
			// If we are here it can mean we have an DVB-API <= 5.4
			SI_LOG_DEBUG("Unable to enumerate the delivery systems, retrying via old API Call");
			auto index = 0;
			switch (_fe_info.type) {
				case FE_QPSK:
					if (_fe_info.caps & FE_CAN_2G_MODULATION) {
						dtvProperty.u.buffer.data[index] = SYS_DVBS2;
						++index;
					}
					dtvProperty.u.buffer.data[index] = SYS_DVBS;
					++index;
					break;
				case FE_OFDM:
					if (_fe_info.caps & FE_CAN_2G_MODULATION) {
						dtvProperty.u.buffer.data[index] = SYS_DVBT2;
						++index;
					}
					dtvProperty.u.buffer.data[index] = SYS_DVBT;
					++index;
					break;
				case FE_QAM:
#  if FULL_DVB_API_VERSION >= 0x0505
					dtvProperty.u.buffer.data[index] = SYS_DVBC_ANNEX_A;
#  else
					dtvProperty.u.buffer.data[index] = SYS_DVBC_ANNEX_AC;
#  endif
					++index;
					break;
				case FE_ATSC:
					if (_fe_info.caps & (FE_CAN_QAM_64 | FE_CAN_QAM_256 | FE_CAN_QAM_AUTO)) {
						dtvProperty.u.buffer.data[index] = SYS_DVBC_ANNEX_B;
						++index;
						break;
					}
				// Fall-through
				default:
					SI_LOG_ERROR("Frontend does not have any known delivery systems");
					CLOSE_FD(fd_fe);
					return false;
			}
			dtvProperty.u.buffer.len = index;
		}
		CLOSE_FD(fd_fe);
#endif
		// get capability of this frontend and count the delivery systems
		for (std::size_t i = 0; i < dtvProperty.u.buffer.len; i++) {
			switch (dtvProperty.u.buffer.data[i]) {
				case SYS_DSS:
					SI_LOG_INFO("Frontend Type: DSS");
					break;
				case SYS_DVBS:
					SI_LOG_INFO("Frontend Type: Satellite (DVB-S)");
					break;
				case SYS_DVBS2:
					// we only count DVB-S2
					++_dvbs2;
					SI_LOG_INFO("Frontend Type: Satellite (DVB-S2)");
					break;
				case SYS_DVBT:
					++_dvbt;
					SI_LOG_INFO("Frontend Type: Terrestrial (DVB-T)");
					break;
				case SYS_DVBT2:
					++_dvbt2;
					SI_LOG_INFO("Frontend Type: Terrestrial (DVB-T2)");
					break;
#if FULL_DVB_API_VERSION >= 0x0505
				case SYS_DVBC_ANNEX_A:
					if (_dvbc == 0) {
						++_dvbc;
					}
					SI_LOG_INFO("Frontend Type: Cable (Annex A)");
					break;
				case SYS_DVBC_ANNEX_C:
					if (_dvbc == 0) {
						++_dvbc;
					}
					SI_LOG_INFO("Frontend Type: Cable (Annex C)");
					break;
#else
				case SYS_DVBC_ANNEX_AC:
					if (_dvbc == 0) {
						++_dvbc;
					}
					SI_LOG_INFO("Frontend Type: Cable (Annex C)");
					break;
#endif
				case SYS_DVBC_ANNEX_B:
					if (_dvbc == 0) {
						++_dvbc;
					}
					SI_LOG_INFO("Frontend Type: Cable (Annex B)");
					break;
				default:
					SI_LOG_INFO("Frontend Type: Unknown %d", dtvProperty.u.buffer.data[i]);
					break;
			}
		}
		SI_LOG_INFO("Frontend Freq: %d Hz to %d Hz", _fe_info.frequency_min, _fe_info.frequency_max);
		SI_LOG_INFO("Frontend srat: %d symbols/s to %d symbols/s", _fe_info.symbol_rate_min, _fe_info.symbol_rate_max);

		// Set delivery systems
		if (_dvbs2 > 0) {
			_deliverySystem.push_back(new input::dvb::delivery::DVBS);
		}
		if (_dvbt > 0 || _dvbt2 > 0) {
			_deliverySystem.push_back(new input::dvb::delivery::DVBT);
		}
		if (_dvbc > 0) {
			_deliverySystem.push_back(new input::dvb::delivery::DVBC);
		}

		return true;
	}

	std::string Frontend::attributeDescribeString(int streamID,
			const input::DeviceData *data) const {
		const input::dvb::FrontendData *frontendData = dynamic_cast<const input::dvb::FrontendData *>(data);
		std::string desc;
		if (frontendData != nullptr) {
			const double freq = frontendData->getFrequency() / 1000.0;
			const int srate = frontendData->getSymbolRate() / 1000;

			const int locked = (_status & FE_HAS_LOCK) ? 1 : 0;

			std::string csv = frontendData->getPidCSV();
			switch (frontendData->getDeliverySystem()) {
				case SYS_DVBS:
				case SYS_DVBS2:
					// ver=1.0;src=<srcID>;tuner=<feID>,<level>,<lock>,<quality>,<frequency>,<polarisation>
					//             <system>,<type>,<pilots>,<roll_off>,<symbol_rate>,<fec_inner>;pids=<pid0>,�,<pidn>
					StringConverter::addFormattedString(desc, "ver=1.0;src=%d;tuner=%d,%d,%d,%d,%.2lf,%c,%s,%s,%s,%s,%d,%s;pids=%s",
							frontendData->getDiSEqcSource(),
							streamID + 1,
							_strength,
							locked,
							_snr,
							freq,
							(frontendData->getPolarization() == POL_V) ? 'v' : 'h',
							StringConverter::delsys_to_string(frontendData->getDeliverySystem()),
							StringConverter::modtype_to_sting(frontendData->getModulationType()),
							StringConverter::pilot_tone_to_string(frontendData->getPilotTones()),
							StringConverter::rolloff_to_sting(frontendData->getRollOff()),
							srate,
							StringConverter::fec_to_string(frontendData->getFEC()),
							csv.c_str());
					break;
				case SYS_DVBT:
				case SYS_DVBT2:
					// ver=1.1;tuner=<feID>,<level>,<lock>,<quality>,<freq>,<bw>,<msys>,<tmode>,<mtype>,<gi>,
					//               <fec>,<plp>,<t2id>,<sm>;pids=<pid0>,�,<pidn>
					StringConverter::addFormattedString(desc, "ver=1.1;tuner=%d,%d,%d,%d,%.2lf,%.3lf,%s,%s,%s,%s,%s,%d,%d,%d;pids=%s",
							streamID + 1,
							_strength,
							locked,
							_snr,
							freq,
							frontendData->getBandwidthHz() / 1000000.0,
							StringConverter::delsys_to_string(frontendData->getDeliverySystem()),
							StringConverter::transmode_to_string(frontendData->getTransmissionMode()),
							StringConverter::modtype_to_sting(frontendData->getModulationType()),
							StringConverter::guardinter_to_string(frontendData->getGuardInverval()),
							StringConverter::fec_to_string(frontendData->getFEC()),
							frontendData->getUniqueIDPlp(),
							frontendData->getUniqueIDT2(),
							frontendData->getSISOMISO(),
							csv.c_str());
					break;
				case SYS_DVBC_ANNEX_B:
#if FULL_DVB_API_VERSION >= 0x0505
				case SYS_DVBC_ANNEX_A:
				case SYS_DVBC_ANNEX_C:
#else
				case SYS_DVBC_ANNEX_AC:
#endif
					// ver=1.2;tuner=<feID>,<level>,<lock>,<quality>,<freq>,<bw>,<msys>,<mtype>,<sr>,<c2tft>,<ds>,
					//               <plp>,<specinv>;pids=<pid0>,�,<pidn>
					StringConverter::addFormattedString(desc, "ver=1.2;tuner=%d,%d,%d,%d,%.2lf,%.3lf,%s,%s,%d,%d,%d,%d,%d;pids=%s",
							streamID + 1,
							_strength,
							locked,
							_snr,
							freq,
							frontendData->getBandwidthHz() / 1000000.0,
							StringConverter::delsys_to_string(frontendData->getDeliverySystem()),
							StringConverter::modtype_to_sting(frontendData->getModulationType()),
							srate,
							frontendData->getC2TuningFrequencyType(),
							frontendData->getDataSlice(),
							frontendData->getUniqueIDPlp(),
							frontendData->getSpectralInversion(),
							csv.c_str());
					break;
				case SYS_UNDEFINED:
					// Not setup yet
					StringConverter::addFormattedString(desc, "NONE");
					break;
				default:
					// Not supported/
					StringConverter::addFormattedString(desc, "NONE");
					break;
			}
		} else {
			StringConverter::addFormattedString(desc, "NONE");
		}
//		SI_LOG_DEBUG("Stream: %d, %s", streamID, desc.c_str());
		return desc;
	}

	// =======================================================================
	//  -- Other member functions --------------------------------------------
	// =======================================================================
	int Frontend::open_fe(const std::string &path, bool readonly) const {
		int fd;
		if ((fd = open(path.c_str(), (readonly ? O_RDONLY : O_RDWR) | O_NONBLOCK)) < 0) {
			PERROR("FRONTEND DEVICE");
		}
		return fd;
	}

	int Frontend::open_dmx(const std::string &path) {
		int fd;
		if ((fd = open(path.c_str(), O_RDWR | O_NONBLOCK)) < 0) {
			PERROR("DMX DEVICE");
		}
		return fd;
	}

	int Frontend::open_dvr(const std::string &path) {
		int fd;
		if ((fd = open(path.c_str(), O_RDONLY | O_NONBLOCK)) < 0) {
			PERROR("DVR DEVICE");
		}
		return fd;
	}

	bool Frontend::set_demux_filter(int fd, uint16_t pid) {
		struct dmx_pes_filter_params pesFilter;

		pesFilter.pid      = pid;
		pesFilter.input    = DMX_IN_FRONTEND;
		pesFilter.output   = DMX_OUT_TS_TAP;
		pesFilter.pes_type = DMX_PES_OTHER;
		pesFilter.flags    = DMX_IMMEDIATE_START;

		if (ioctl(fd, DMX_SET_PES_FILTER, &pesFilter) != 0) {
			PERROR("DMX_SET_PES_FILTER");
			return false;
		}
		return true;
	}

	bool Frontend::tune(const int streamID, const input::dvb::FrontendData &frontendData) {
		const fe_delivery_system_t delsys = frontendData.getDeliverySystem();
		for (input::dvb::delivery::SystemVector::iterator it = _deliverySystem.begin();
		     it != _deliverySystem.end();
		     ++it) {
			if ((*it)->isCapableOf(delsys)) {
				return (*it)->tune(streamID, _fd_fe, frontendData);
			}
		}
		return false;
	}

	bool Frontend::setupAndTune(const int streamID, const input::dvb::FrontendData &frontendData) {
		if (!_tuned) {
			// Check if we have already opened a FE
			if (_fd_fe == -1) {
				_fd_fe = open_fe(_path_to_fe, false);
				SI_LOG_INFO("Stream: %d, Opened %s fd: %d", streamID, _path_to_fe.c_str(), _fd_fe);
			}
			// try tuning
			std::size_t timeout = 0;
			while (!tune(streamID, frontendData)) {
				usleep(450000);
				++timeout;
				if (timeout > 3) {
					return false;
				}
			}
			SI_LOG_INFO("Stream: %d, Waiting on lock...", streamID);

			// check if frontend is locked, if not try a few times
			timeout = 0;
			while (timeout < 4) {
				fe_status_t status = FE_TIMEDOUT;
				// first read status
				if (ioctl(_fd_fe, FE_READ_STATUS, &status) == 0) {
					if (status & FE_HAS_LOCK) {
						// We are tuned now
						_tuned = true;
						SI_LOG_INFO("Stream: %d, Tuned and locked (FE status 0x%X)", streamID, status);
						break;
					} else {
						SI_LOG_INFO("Stream: %d, Not locked yet   (FE status 0x%X)...", streamID, status);
					}
				}
				usleep(150000);
				++timeout;
			}
		}
		// Check if we have already a DVR open and are tuned
		if (_fd_dvr == -1 && _tuned) {
			// try opening DVR, try again if fails
			std::size_t timeout = 0;
			while ((_fd_dvr = open_dvr(_path_to_dvr)) == -1) {
				usleep(150000);
				++timeout;
				if (timeout > 3) {
					return false;
				}
			}
			SI_LOG_INFO("Stream: %d, Opened %s fd: %d", streamID, _path_to_dvr.c_str(), _fd_dvr);

			{
				base::MutexLock lock(_mutex);
				if (ioctl(_fd_dvr, DMX_SET_BUFFER_SIZE, _dvrBufferSize) == -1) {
					PERROR("DMX_SET_BUFFER_SIZE failed");
				}
			}
		}
		return (_fd_dvr != -1) && _tuned;
	}

	void Frontend::resetPid(int pid, input::dvb::FrontendData &frontendData) {
		if (frontendData.getDMXFileDescriptor(pid) != -1 &&
			ioctl(frontendData.getDMXFileDescriptor(pid), DMX_STOP) != 0) {
			PERROR("DMX_STOP");
		}
		frontendData.closeDMXFileDescriptor(pid);
		frontendData.resetPid(pid);
	}

	bool Frontend::updatePIDFilters(const int streamID, input::dvb::FrontendData &frontendData) {
		if (frontendData.hasPIDTableChanged()) {
			frontendData.resetPIDTableChanged();
			SI_LOG_INFO("Stream: %d, Updating PID filters...", streamID);
			int i;
			for (i = 0; i < MAX_PIDS; ++i) {
				// check if PID is used or removed
				if (frontendData.isPIDUsed(i)) {
					// check if we have no DMX for this PID, then open one
					if (frontendData.getDMXFileDescriptor(i) == -1) {
						frontendData.setDMXFileDescriptor(i, open_dmx(_path_to_dmx));
						std::size_t timeout = 0;
						while (set_demux_filter(frontendData.getDMXFileDescriptor(i), i) != 1) {
							usleep(350000);
							++timeout;
							if (timeout > 3) {
								return false;
							}
						}
						SI_LOG_DEBUG("Stream: %d, Set filter PID: %04d - fd: %03d%s",
								streamID, i, frontendData.getDMXFileDescriptor(i), frontendData.isPMT(i) ? " - PMT" : "");
					}
				} else if (frontendData.getDMXFileDescriptor(i) != -1) {
					// We have a DMX but no PID anymore, so reset it
					SI_LOG_DEBUG("Stream: %d, Remove filter PID: %04d - fd: %03d - Packet Count: %d",
							streamID, i, frontendData.getDMXFileDescriptor(i), frontendData.getPacketCounter(i));
					resetPid(i, frontendData);
				}
			}
		}
		return true;
	}

} // namespace dvb
} // namespace input
