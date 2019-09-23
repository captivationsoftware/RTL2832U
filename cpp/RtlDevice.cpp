/*
 * This file is protected by Copyright. Please refer to the COPYRIGHT file
 * distributed with this source distribution.
 *
 * This file is part of RTL2832U Device.
 *
 * RTL2832U Device is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * RTL2832U Device is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

#include "RtlDevice.h"
PREPARE_LOGGING(RtlDevice)

#include <cmath>
#include <cstring>
#include <iostream>

#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

RtlDevice::RtlDevice(uint32_t channelNumber)
{
    // Initialize the pointer
    m_device = NULL;

    m_channelNumber = channelNumber;

    try {
        // Allocate memory for buffers and initialize indices
        m_buffer = new uint8_t * [m_NUM_BUFFERS];

        for (uint32_t i=0; i < m_NUM_BUFFERS; ++i) {
            m_buffer[i] = new uint8_t[m_BUFFER_LEN];
        }

        m_curRead = 0;
        m_curWrite = 0;
        m_curIndex = 0;

        // Initialize flags and thread data
        m_startWriting = false;
        m_dataRecvThread = NULL;

        if (rtlsdr_open(&m_device, m_channelNumber) >= 0) {
            if (rtlsdr_get_device_usb_strings(m_channelNumber, m_vendor, m_product, m_serial)) {
                LOG_INFO(RtlDevice, "Unable to get device USB strings of index " << m_channelNumber);
            }

            strncpy(m_deviceName, rtlsdr_get_device_name(m_channelNumber), 256);

            if (m_deviceName) {
                LOG_INFO(RtlDevice, "Using device " << m_deviceName << " at index " << m_channelNumber);
            } else {
                LOG_INFO(RtlDevice, "Unable to get device name of index " << m_channelNumber);
            }

            // Set the frequency range based upon the tuner type
            switch (rtlsdr_get_tuner_type(m_device)) {
                case RTLSDR_TUNER_E4000:
                    m_frequencyRange.setStart(E4000_MIN_CF);
                    m_frequencyRange.setStop(E4000_MAX_CF);
                    break;
                case RTLSDR_TUNER_FC0012:
                    m_frequencyRange.setStart(FC0012_MIN_CF);
                    m_frequencyRange.setStop(FC0012_MAX_CF);
                    break;
                case RTLSDR_TUNER_FC0013:
                    m_frequencyRange.setStart(FC0013_MIN_CF);
                    m_frequencyRange.setStop(FC0013_MAX_CF);
                    break;
                case RTLSDR_TUNER_FC2580:
                    m_frequencyRange.setStart(FC2580_MIN_CF);
                    m_frequencyRange.setStop(FC2580_MAX_CF);
                    break;
                case RTLSDR_TUNER_R820T:
                case RTLSDR_TUNER_R828D:
                    m_frequencyRange.setStart(R82XX_MIN_CF);
                    m_frequencyRange.setStop(R82XX_MAX_CF);
                    break;
                default:
                    LOG_WARN(RtlDevice, "Unable to determine tuner type");
                    // Simply specify the FM Broadcast Range
                    m_frequencyRange.setStart(FM_BROADCAST_MIN_CF);
                    m_frequencyRange.setStop(FM_BROADCAST_MAX_CF);
            }

            // Set the sample rate range
            m_rateRange.setStart(RTL_MIN_SR);
            m_rateRange.setStop(RTL_MAX_SR);

            // Set the gain range
            int numGains = rtlsdr_get_tuner_gains(m_device, NULL);

            if (numGains > 0) {
                int *gains = new int[numGains];
                double min = INFINITY, max = -INFINITY;

                rtlsdr_get_tuner_gains(m_device, gains);

                for (int i=0; i < numGains; ++i) {
                    if (gains[i] < min) {
                        min = gains[i];
                    }

                    if (gains[i] > max) {
                        max = gains[i];
                    }
                }

                delete[] gains;

                m_gainRange.setStart(min / 10.0);
                m_gainRange.setStop(max / 10.0);
            } else {
                LOG_WARN(RtlDevice, "Unable to get tuner gains");
            }

            // Give the device an initial sample rate
            rtlsdr_set_sample_rate(m_device, m_rateRange.start());
        } else {
            LOG_WARN(RtlDevice, "Unable to open device");
            m_device = NULL;
        }
    } catch (std::bad_alloc &e) {
        LOG_ERROR(RtlDevice, "Bad Memory Allocation: " << e.what());
    }
}

/* This function allows the user to pass it the addresses of two buffers
 * and the number of elements to read.  It returns the number of
 * elements read
 */
uint32_t RtlDevice::recv(float *floatOutputBuffer, uint8_t *octetOutputBuffer, uint32_t maxLength)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    // You must issue a start command before attempting to read data
    if (not m_dataRecvThread) {
        LOG_INFO(RtlDevice, "Unable to receive data: Streaming not started");
        return 0;
    } else if (not m_device) {
        LOG_WARN(RtlDevice, "Unable to receive data: Could not open RTL device "<<m_channelNumber);
        return 0;
    }

    if (floatOutputBuffer && octetOutputBuffer) {
        uint32_t numRead = 0;

        // This loop will read data from the internal buffer until the specified
        // length has been reached.  It also waits to make sure it isn't getting
        // ahead of the incoming data from the RTL device
        while (numRead < maxLength) {
            {
                boost::mutex::scoped_lock lock(m_readLock);

                while (m_curRead == m_curWrite) {  // Probably don't need while loop since only one thread is waiting for the condition
                    const boost::system_time timeout = boost::get_system_time() + boost::posix_time::milliseconds(500);

                    if (not m_dataCondition.timed_wait(lock, timeout)) {
                        if (m_dataRecvThread) {    // The device reading thread seems to have hung
                            LOG_ERROR(RtlDevice, "Unexpected condition timeout while waiting to receive more data");
                        } else { // The device reading thread was stopped and no condition was signalled
                            LOG_INFO(RtlDevice, "The device was stopped while waiting to receive more data");
                        }

                        return numRead;
                    }

                    if (not m_dataRecvThread) { // The device reading thread was stopped and a condition was signalled
                        return numRead;
                    }
                }

                for (; m_curIndex < m_BUFFER_LEN && numRead < maxLength; ++m_curIndex, ++numRead) {
                    octetOutputBuffer[numRead] = m_buffer[m_curRead][m_curIndex];
                    floatOutputBuffer[numRead] = (float) (octetOutputBuffer[numRead] / 127.5 - 1);
                }

                if (m_curIndex == m_BUFFER_LEN) {
                    m_curRead = (m_curRead + 1) % m_NUM_BUFFERS;
                    m_curIndex = 0;
                }
            }
        }

        return numRead;
    } else {
        LOG_WARN(RtlDevice, "Unable to write to output buffer: Output buffer is NULL");
    }

    return 0;
}

/* This function starts and stops the asynchronous reading of data from
 * the dongle
 */
void RtlDevice::issueStreamCmd(stream_cmd_t cmd)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (not m_device) {
        LOG_WARN(RtlDevice, "Unable to issue stream command: Could not open RTL device "<<m_channelNumber);
        return;
    }
    // For error checking
    int r;

    // Check which command we got
    switch (cmd) {
        case STREAM_MODE_START_CONTINUOUS: {
            if (m_dataRecvThread) {
                LOG_DEBUG(RtlDevice, "Unable to start streaming: Already streaming");
                return;
            }

            // Initialize the read, write, and current indices
            m_curRead = 0;
            m_curWrite = 0;
            m_curIndex = 0;

            m_dataRecvThread = new boost::thread(&RtlDevice::threadFunction, this);

            m_startWriting = true;

            LOG_INFO(RtlDevice, "Streaming started");

            break;
        }
        case STREAM_MODE_STOP_CONTINUOUS: {
            if (not m_dataRecvThread) {
                LOG_DEBUG(RtlDevice, "Unable to stop streaming: Already stopped");
                return;
            }

            // Cancel the asynchronous read, wait for the thread to finish, and destroy
            // the mutex lock and condition variable
            int i = 0;
            while ((r = rtlsdr_cancel_async(m_device))) {
                usleep(500000);
                ++i;

                if (i == 5) {
                    LOG_FATAL(RtlDevice, "Unable to cancel asynchronous read with error " << r);
                    exit(1);
                }
            }

            m_startWriting = false;

            m_dataCondition.notify_all();

            m_dataRecvThread->join();

            delete m_dataRecvThread;
            m_dataRecvThread = NULL;

            LOG_INFO(RtlDevice, "Streaming stopped");

            break;
        }
        default:
            LOG_WARN(RtlDevice, "Invalid stream command");
    }
}

/* Attempt to set the RTL2832 gain mode.
 */
void RtlDevice::setAgcMode(bool enable)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (m_device) {
        // For error checking
        int r;

        if ((r = rtlsdr_set_agc_mode(m_device, (int) enable)) != 0) {
            LOG_WARN(RtlDevice, "Unable to set RTL2832 agc mode to " << (enable ? "enabled" : "disabled") << " with error " << r);
        } else {
            LOG_INFO(RtlDevice, "RTL2832 agc mode set to " << (enable ? "enabled" : "disabled"));
        }
    } else {
        LOG_WARN(RtlDevice, "Unable to set RTL2832 agc mode: Could not open RTL device "<<m_channelNumber);
    }
}

/* Attempt to set the RTL2832 bias mode.
 */
void RtlDevice::setBiasMode(bool enable)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (m_device) {
        // For error checking
        int r;

        if ((r = rtlsdr_set_bias_tee(m_device, (int) enable)) != 0) {
            LOG_WARN(RtlDevice, "Unable to set RTL2832 bias mode to " << (enable ? "enabled" : "disabled") << " with error " << r);
        } else {
            LOG_INFO(RtlDevice, "RTL2832 bias mode set to " << (enable ? "enabled" : "disabled"));
        }
    } else {
        LOG_WARN(RtlDevice, "Unable to set RTL2832 bias mode: Could not open RTL device "<<m_channelNumber);
    }
}

/* Attempt to set the tuner gain mode.
 */
void RtlDevice::setGainMode(bool automatic)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (m_device) {
        // For error checking
        int r;

        if ((r = rtlsdr_set_tuner_gain_mode(m_device, int(!automatic))) != 0) {
            LOG_WARN(RtlDevice, "Unable to set tuner gain mode to " << (automatic ? "auto" : "manual") << " with error " << r);
        } else {
            LOG_INFO(RtlDevice, "Tuner gain mode set to " << (automatic ? "auto" : "manual"));
        }
    } else {
        LOG_WARN(RtlDevice, "Unable to set gain mode: Could not open RTL device "<<m_channelNumber);
    }
}

/* Attempt to set the tuner gain.  Note that the gain is
 * specified in decibels.
 * Tuner gain mode must be set to manual for this setter to work.
 */
void RtlDevice::setGain(double gain)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (m_device) {
        // For error checking
        int r;

        // round is used to handle floating-point math errors
        if ((r = rtlsdr_set_tuner_gain(m_device, (int) round(gain * 10))) != 0) {
            LOG_WARN(RtlDevice, "Unable to set tuner gain to " << gain << " dB with error " << r);
        } else {
            LOG_INFO(RtlDevice, "Tuner gain set to " << gain);
        }
    } else {
        LOG_WARN(RtlDevice, "Unable to set gain: Could not open RTL device "<<m_channelNumber);
    }
}

/* Get the gain and convert it to decibels
 */
double RtlDevice::getGain()
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (m_device) {
        int gain;

        if ((gain = rtlsdr_get_tuner_gain(m_device)) == 0) {
            // rtlsdr_get_tuner_gain returns 0 if m_device is null, otherwise it returns the gain value
            // the gain value could also be 0, so it's unclear when an error occurs based solely on return value
            if (!m_device)
                LOG_WARN(RtlDevice, "Unable to get gain");

            return 0.0;
        }

        return (double) gain / 10.0;
    } else {
        LOG_WARN(RtlDevice, "Unable to get gain: Could not open RTL device "<<m_channelNumber);
    }

    return 0.0;
}

/* Get the frequency range for the tuner, or return the
 * FM broadcast range if the range is unavailable
 */
FrequencyRange RtlDevice::getFreqRange()
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    return m_frequencyRange;
}

/* Get the sample rate range for the tuner
 */
RateRange RtlDevice::getRateRange()
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    return m_rateRange;
}

/* Get the gain range for the tuner, or return a default
 * gain of 1 if the range is unavailable
 */
GainRange RtlDevice::getGainRange()
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    return m_gainRange;
}

/* Attempt to set the center frequency of the tuner
 */
void RtlDevice::setFreq(double freq)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    // librtlsdr accepts frequency as an integer. To ensure we tune as near to
    // the request as possible, we'll use round rather than truncating
    freq = round(freq);

    if (m_device) {
        // For error checking
        int r;

        if (freq < m_frequencyRange.start()) {
            LOG_WARN(RtlDevice, "Desired center frequency " << freq << " Hz less than minimum of " << m_frequencyRange.start() << " Hz");
            freq = m_frequencyRange.start();
        } else if (freq > m_frequencyRange.stop()) {
            LOG_WARN(RtlDevice, "Desired center frequency " << freq << " Hz greater than maximum of " << m_frequencyRange.stop() << " Hz");
            freq = m_frequencyRange.stop();
        }

        bool done = false;
        unsigned int attempt=0;
        while (not done) {
            if ((r = rtlsdr_set_center_freq(m_device, (uint32_t) freq)) < 0) {
                // This fails occasionally when RTL is being controlled from within a virtual machine.
                // Experimentation shows that this will typically succeed on the second attempt.
                attempt++;
                if (attempt < 50){ // keep trying for up to 5 seconds
                    LOG_INFO(RtlDevice, "Error setting frequency, trying again...");
                    usleep(100000); // sleep 100 ms
                } else {
                    LOG_WARN(RtlDevice, "Unable to set frequency to " << freq << " Hz with error " << r);
                    done = true;
                }
            } else {
                done = true;
                LOG_INFO(RtlDevice, "Center frequency set to " << freq << " Hz");
            }
        }
    } else {
        LOG_WARN(RtlDevice, "Unable to set frequency: Could not open RTL device "<<m_channelNumber);
    }
}

/* Attempt to get the center frequency of the tuner, or
 * return a default value of 0 if the center frequency
 * is unavailable
 */
double RtlDevice::getFreq()
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (m_device) {
        uint32_t freq;

        bool done = false;
        unsigned int attempt = 0;
        while (not done) {
            if ((freq = rtlsdr_get_center_freq(m_device))) {
                return (double) freq;
            } else {
                // This fails occasionally when RTL is being controlled from within a virtual machine.
                // Experimentation shows that this will typically succeed on the second attempt.
                attempt++;
                if (attempt < 50){ // keep trying for up to 5 seconds
                    LOG_INFO(RtlDevice, "Error getting frequency, trying again...");
                    usleep(100000); // sleep 100 ms
                } else {
                    LOG_WARN(RtlDevice, "Unable to get tuner frequency with error " << freq);
                    done = true;
                }
            }
        }
    } else {
        LOG_WARN(RtlDevice, "Unable to get frequency: Could not open RTL device "<<m_channelNumber);
    }

    return 0.0;
}

/* Attempt to set the sample rate of the tuner
 */
void RtlDevice::setRate(double rate)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    // librtlsdr accepts rate as an integer. To ensure we have a sample rate
    // that meets or exceeds the request, we'll use ceil rather than truncating
    rate = ceil(rate);

    if (m_device) {
        if (rate < m_rateRange.start()) {
            LOG_WARN(RtlDevice, "Desired sample rate " << rate << " sps less than minimum of " << m_rateRange.start() <<  " sps");
            rate = m_rateRange.start();
        } else if (rate > m_rateRange.stop()) {
            LOG_WARN(RtlDevice, "Desired sample rate " << rate << " sps greater than maximum of " << m_rateRange.stop() << " sps");
            rate = m_rateRange.stop();
        }

        if (rtlsdr_set_sample_rate(m_device, (uint32_t) rate) < 0) {
            LOG_WARN(RtlDevice, "Unable to set sample rate to " << rate << " sps");
        } else {
            LOG_INFO(RtlDevice, "Sample rate set to " << rate << " sps");
        }
    } else {
        LOG_WARN(RtlDevice, "Unable to set sample rate: Could not open RTL device "<<m_channelNumber);
    }
}

/* Attempt to get the sample rate of the tuner
 */
double RtlDevice::getRate()
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (m_device) {
        uint32_t rate = rtlsdr_get_sample_rate(m_device);

        if (rate == 0) {
            LOG_WARN(RtlDevice, "Unable to get sample rate");
        }

        return rate;
    } else {
        LOG_WARN(RtlDevice, "Unable to get sample rate: Could not open RTL device "<<m_channelNumber);
    }

    return 0.0;
}

/* Return the crystal oscillator frequencies used for the RTL2832 and the tuner IC.
 * The first element is the frequency value used to clock the RTL2832 in Hz
 * The second element is the frequency value used to clock the tuner IC in Hz
 * Returns empty vector on error
 */
std::vector<double> RtlDevice::getClockRates()
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    std::vector<double> rates;
    uint32_t rtlFreq;
    uint32_t tunerFreq;

    if (m_device) {
        uint32_t r;

        if ((r = rtlsdr_get_xtal_freq(m_device,&rtlFreq,&tunerFreq))) {
            LOG_WARN(RtlDevice, "Unable to get clock rates");
        } else {
            rates.push_back(double(rtlFreq));
            rates.push_back(double(tunerFreq));
        }

    } else {
        LOG_WARN(RtlDevice, "Unable to get clock rate: Could not open RTL device "<<m_channelNumber);
    }

    return rates;
}

/*!
 * Set the frequency correction value for the device.
 * \param ppm correction value in parts per million (ppm)
 */
void RtlDevice::setFreqCorrection(int ppm){
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);
    if (m_device) {
        uint32_t r;
        if((r = rtlsdr_set_freq_correction(m_device,ppm)) != 0){
            if(ppm != rtlsdr_get_freq_correction(m_device)){
                LOG_WARN(RtlDevice, "Unable to set frequency correction value");
            } else {
                LOG_INFO(RtlDevice, "Set frequency correction value to "<<ppm);
            }
        } else {
            LOG_INFO(RtlDevice, "Set frequency correction value to "<<ppm);
        }
    } else {
        LOG_WARN(RtlDevice, "Unable to set frequency correction: Could not open RTL device "<<m_channelNumber);
    }
}

/*!
 * Get actual frequency correction value of the device.
 * \return correction value in parts per million (ppm)
 */
int RtlDevice::getFreqCorrection(){
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);
    if (m_device) {
        return rtlsdr_get_freq_correction(m_device);
    } else {
        LOG_WARN(RtlDevice, "Unable to get frequency correction: Could not open RTL device "<<m_channelNumber);
    }
    return 0;
}

/* Copy the usb strings into the given pointers.  It's up
 * to the user to guarantee at least 256 characters for each
 */
void RtlDevice::getUsbStrings(char *vendor, char *product, char *serial)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (vendor) {
        strncpy(vendor, m_vendor, 256);
    }

    if (product) {
        strncpy(product, m_product, 256);
    }

    if (serial) {
        strncpy(serial, m_serial, 256);
    }
}

/* Copy the device name into the given pointer.  It's up
 * to the user to guarantee at least 256 characters
 */
void RtlDevice::getName(char *name)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (name) {
        strncpy(name, m_deviceName, 256);
    }
}

/* The function passed to the boost::thread object.  This
 * method simply resets the asynchronous buffer and calls
 * the blocking asynchronous read function
 */
void RtlDevice::threadFunction()
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    if (m_device) {
        rtlsdr_reset_buffer(m_device);

        if (rtlsdr_read_async(m_device, &RtlDevice::rtlCallback, (void *) this, m_NUM_BUFFERS, m_BUFFER_LEN)) {
            LOG_ERROR(RtlDevice, "Unable to read data");
        }
    } else {
        LOG_WARN(RtlDevice, "Unable to read data: Could not open RTL device "<<m_channelNumber);
    }
}

/* This is the callback function for the asynchronous read
 * function.  It simply locks the mutex and copies the data
 * from the device to the intermediate buffer.  It also
 * checks to make sure that the read index and the write
 * index are not the same and signals if this condition is
 * true
 */
void RtlDevice::rtlCallback(uint8_t *buf, uint32_t len, void *ctx)
{
    LOG_TRACE(RtlDevice, __PRETTY_FUNCTION__);

    RtlDevice *device = (RtlDevice *) ctx;

    if (device == NULL) {
        LOG_WARN(RtlDevice, "Unable to receive data: Object is null");
        return;
    }

    if (not device->m_startWriting) {
        return;
    }

    boost::mutex::scoped_lock lock(device->m_readLock);

    memcpy(device->m_buffer[device->m_curWrite], buf, len);
    device->m_curWrite = (device->m_curWrite + 1) % m_NUM_BUFFERS;

    if (device->m_curRead != device->m_curWrite) {
        device->m_dataCondition.notify_one();
    }
}

/* Clean up the device
 */
RtlDevice::~RtlDevice()
{
    if (m_dataRecvThread) {
        issueStreamCmd(STREAM_MODE_STOP_CONTINUOUS);
    }

    if (m_buffer != NULL) {
        for (uint32_t i=0; i < m_NUM_BUFFERS; ++i) {
            delete[] m_buffer[i];
        }

        delete[] m_buffer;
    }

    if (rtlsdr_close(m_device)) {
        LOG_WARN(RtlDevice, "Unable to close RTL device " << rtlsdr_get_device_name(m_channelNumber));
    }
}
