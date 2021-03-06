/*
 * VantageVue Connect - A small program to connect a vantage vue console
 * Copyright (C) 2015  BRUNEAUX Jerome <jbruneaux@laposte.net>
 *
 * This file is part of VantageVueConnect.
 *
 * VantageVueConnect is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * any later version.
 *
 * VantageVueConnect is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with VantageVueConnect.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <sys/time.h>
#include <sys/types.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <sys/poll.h>
#include <sys/timerfd.h>

#include "vantage_serial.h"
#include "vantage_serial_prot.h"
#include "data_defs.h"
#include "log.h"

#define MAX_DEV_PATH_LEN 64

#define TTY_RECV_BUFFER_SIZE (LOOP_PACKET_SIZE * 2)

#define LOOP_PACKET_REQ   "LOOP 1\n"
#define LOOP2_PACKET_REQ  "LPS 2 1\n"

extern int loglevel;
extern int use_loop2;

typedef enum vantage_console_decoder_state_e
{
  VTG_DECODER_STATE_WAIT_ANY,

  /* LOOPs message decoding */
  VTG_DECODER_STATE_WAIT_LOOP_PARTIAL_HEADER,
  VTG_DECODER_STATE_WAIT_LOOP_PACKET_DATA_END,


} vantage_console_decoder_state_t;

typedef struct vantage_console_context_s
{
  int  use_usb_serial;
  char console_dev_path[MAX_DEV_PATH_LEN];
  int  console_tty_fd;
  int  timer_fd;

  vantage_console_decoder_state_t decoder_state;
  int                             message_awaited_length;
  int                             data_recvd_length;
  unsigned char                   decoder_message_buffer[TTY_RECV_BUFFER_SIZE];

  volatile int  tty_thread_stop_req;
  pthread_t     tty_reader_thread;

  VTG_DataReadyIndicateCb_t *DataReadyIndicateCb;
  
} vantage_console_context_t;

static vantage_console_context_t vantage_console_priv = {0} ;


/*******************************************************************************
 * Private functions
 ******************************************************************************/
static int console_tty_open(char* dev_path)
{
  int tmp_file_fd;
  struct termios t;

  LOG_printf(LOG_LVL_WARNING, "Try to open console on device %s\n", dev_path);

  tmp_file_fd = open (dev_path, O_RDWR | O_NONBLOCK);
  if (tmp_file_fd < 0)
  {
    LOG_printf(LOG_LVL_WARNING, "Can't open device %s. Errno %d\n", dev_path, errno);
    return -1;
  }

  /* Get current attributes */
  tcgetattr(tmp_file_fd, &t);

  /* Set raw mode */
  cfmakeraw(&t);
  
  /* Set Baudrate */
  cfsetospeed(&t, B19200);
  cfsetispeed(&t, B19200);

  /* Flush IO */
  tcflush(tmp_file_fd, TCIOFLUSH);

  /* Apply changes */
  tcsetattr(tmp_file_fd, TCSANOW, &t);

  return tmp_file_fd;
}

static int console_wakeup(int console_tty_fd)
{
  fd_set fdset;
  struct timeval tv;
  int retval;
  unsigned char tmp_buf[2];
  unsigned char *pbuf;
  int console_awake = 0;
  int cr_lf = 0;
  int wake_attemp_counter = 3;

  if (loglevel > 1)
  {
    LOG_printf(LOG_LVL_DEBUG, "console_wakeup\n");
  }

  while ( (wake_attemp_counter--) && (!console_awake) )
  {
    write(console_tty_fd, "\n", 1);

    FD_ZERO(&fdset);
    FD_SET(console_tty_fd, &fdset);

    tv.tv_sec = 1;
    tv.tv_usec = 0;

    if (loglevel > 1)
    {
      LOG_printf(LOG_LVL_DEBUG, "console_wakeup -> select\n");
    }

    retval = select(console_tty_fd+1, &fdset, NULL, NULL, &tv);

    if (loglevel > 1)
    {
      LOG_printf(LOG_LVL_DEBUG, "console_wakeup select returns %d\n", retval);
    }

    if (retval)
    {
      retval = read(console_tty_fd, tmp_buf, 2);
      pbuf = tmp_buf;
      while(retval--)
      {
        if ((*pbuf == '\r') && (cr_lf == 0))
        {
          cr_lf = 1;
        }
        else if ((*pbuf == '\n') && (cr_lf == 1))
        {
          console_awake = 1;
        }
        else
        {
          cr_lf = 0;
        }

        pbuf++;
      }
    }
    else
    {
      LOG_printf(LOG_LVL_ERROR, "No answer received\n");
    }
  }


  return console_awake;
}

static int console_tty_probe(char* dev_path, int autodetect)
{
  int tmp_file_fd, wakeup_result;
  char tmp_dev_path[MAX_DEV_PATH_LEN];
  int devno = 0;

  if (autodetect)
  {
    for (devno=0; devno<10; devno++)
    {
      snprintf(tmp_dev_path, MAX_DEV_PATH_LEN, "%s%d", dev_path, devno);
      LOG_printf(LOG_LVL_INFO, "Look-up for console on device %s\n", tmp_dev_path);
      tmp_file_fd = console_tty_open(tmp_dev_path);
      if (tmp_file_fd != -1)
      {
        wakeup_result = console_wakeup(tmp_file_fd);
        if (wakeup_result == 1)
        {
          LOG_printf(LOG_LVL_INFO, "Found console on device %s\n", tmp_dev_path);
          break;
        }
        else
        {
          close(tmp_file_fd);
          tmp_file_fd = -1;
        }
      }
    }
  }
  else
  {
    tmp_file_fd = console_tty_open(dev_path);
    if (tmp_file_fd != -1)
    {
      wakeup_result = console_wakeup(tmp_file_fd);
      if (wakeup_result == 1)
      {
        return tmp_file_fd;
      }
      else
      {
        close(tmp_file_fd);
        tmp_file_fd = -1;
      }
    }
  }

  return tmp_file_fd;
}

static void console_tty_close(int console_tty_fd)
{
  close(console_tty_fd);
}

static int console_check_crc(unsigned char* buf, int size)
{
  int       index = 0;
  uint16_t  crc = 0;

  for( index = 0; index < size; index ++ )
  {
      crc =  crc_table [( crc >> 8 ) ^ buf[index]] ^ ( crc << 8 );
  }

  return (crc == 0) ? 1 : 0;
}

static float compute_wind_chill(float air_temperature_F, float wind_speed_MPH)
{
  if ((air_temperature_F >= -50.0) && (air_temperature_F <= 50.0) && 
       (wind_speed_MPH >= 3.0) && (wind_speed_MPH <= 110.0))
  {
    return (35.74 + (0.6215 * air_temperature_F) - (35.75 * powf(wind_speed_MPH, 0.16)) + (0.4275 * air_temperature_F * powf(wind_speed_MPH, 0.16)));
  }
  else
  {
    return (air_temperature_F - (1.5 * wind_speed_MPH));
  }
}

static float compute_dew_point(float air_temperature_F, int humidity)
{
  float air_temperature_C = (air_temperature_F - 32.0) / 1.8;
  float a=17.271;
  float b=273.3;

  float humf = humidity / 100.0;
  float gamma = ((a * air_temperature_C) / (b + air_temperature_C)) + log(humf);
  float dewc = (b*gamma) / (a-gamma);
  
  return ((dewc * 1.8) + 32);
}

static void console_process_loop_message(vantage_console_context_t *vantage_console_priv)
{
  vantage_loop_packets_t* packet_buffer = (vantage_loop_packets_t*) vantage_console_priv->decoder_message_buffer;
  weather_data_t          weather_data = {0};

  time_t t = time(NULL);
  weather_data.tm = *localtime(&t);

  /* First, check the packet type byte to know if it's a LOOP or LOOP2 message
   * As the first message fields are in the same layout, we use the LOOP message
   * definition to access the packet type information
   */

  /* LOOP message */
  if (packet_buffer->loop_packet.packet_type == 0)
  {
    if (loglevel > 0)
    {
      LOG_printf(LOG_LVL_INFO, "Received LOOP packet\n");
    }

    /* Outisde temperature is in 10th of Fahrenheit. Convert it to Celcius */
    weather_data.outside_temperature_F = (float) BSWAP16(packet_buffer->loop_packet.OutsideTemp) / 10.0;
    weather_data.outside_temperature_C = (weather_data.outside_temperature_F - 32.0) / 1.8;

    /* Outside Humidity is in percent */
    weather_data.outside_humidity = packet_buffer->loop_packet.OutsideHumidity;

    /* Wind speed is in Miles per hour */
    weather_data.wind_speed_MPH = packet_buffer->loop_packet.WindSpeed;
    weather_data.wind_speed_KPH = 1.609344 * weather_data.wind_speed_MPH;

    /* Wind chill is in fahrenheit. Convert it to Celcius */
    weather_data.outside_chill_F = compute_wind_chill(weather_data.outside_temperature_F, weather_data.wind_speed_MPH);
    weather_data.outside_chill_C = ((float) weather_data.outside_chill_F - 32.0) / 1.8;
    
    /* Dew point is in fahrenheit. Convert it to Celcius */
    weather_data.dew_point_F = compute_dew_point(weather_data.outside_temperature_F, weather_data.outside_humidity);
    weather_data.dew_point_C = (weather_data.dew_point_F - 32.0) / 1.8;

    /* Inside temperature is in 10th of Fahrenheit. Convert it to Celcius */
    weather_data.inside_temperature_F = (float) BSWAP16(packet_buffer->loop_packet.InsideTemp) / 10.0;
    weather_data.inside_temperature_C = (weather_data.inside_temperature_F - 32.0) / 1.8;

    /* Inside Humidity is in percent */
    weather_data.inside_humidity = packet_buffer->loop_packet.InsideHumidity;

    /* Barometric pressure is in inches. Convert to mBar */
    weather_data.barometric_pressure_I = BSWAP16(packet_buffer->loop_packet.Barometer);
    weather_data.barometric_pressure_Hpa = (33.8639 * weather_data.barometric_pressure_I) / 1000.0;

    /* Rain rate is in rain clicks (TODO : retrieve customized rain sensor value ?) */
    weather_data.rain_rate_I = 0.01 * BSWAP16(packet_buffer->loop_packet.RainRate);
    weather_data.rain_rate_MM = 0.2 * BSWAP16(packet_buffer->loop_packet.RainRate);

    /* Day rain is in rain clicks */
    weather_data.rain_day_I = 0.01 * BSWAP16(packet_buffer->loop_packet.DayRain);
    weather_data.rain_day_MM = 0.2 * BSWAP16(packet_buffer->loop_packet.DayRain);

    /* Wind speed average is in 0.1mph */
    weather_data.wind_speed_avg_2m_MPH = 0.1 * packet_buffer->loop_packet.TenMinuteAvgWindSpeed;
    weather_data.wind_speed_avg_2m_KPH = 1.609344 * weather_data.wind_speed_avg_2m_MPH;

    weather_data.wind_direction = BSWAP16(packet_buffer->loop_packet.WindDir);

    /* Wind gust is in 0.1mph */
    weather_data.wind_gust = FLT_MIN;
    weather_data.wind_gust_10m = FLT_MIN;

    weather_data.wind_direction_gust_10m = FLT_MIN;
  }
  /* LOOP2 message */
  else if (packet_buffer->loop_packet.packet_type == 1)
  {
    if (loglevel > 0)
    {
      LOG_printf(LOG_LVL_INFO, "Received LOOP2 packet\n");
    }

    /* Outisde temperature is in 10th of Fahrenheit. Convert it to Celcius */
    weather_data.outside_temperature_F = (float) BSWAP16(packet_buffer->loop2_packet.OutsideTemp) / 10.0;
    weather_data.outside_temperature_C = (weather_data.outside_temperature_F - 32.0) / 1.8;

    /* Outside Humidity is in percent */
    weather_data.outside_humidity = packet_buffer->loop2_packet.OutsideHumidity;

    /* Wind chill is in fahrenheit. Convert it to Celcius */
    weather_data.outside_chill_F = BSWAP16(packet_buffer->loop2_packet.WindChill);
    weather_data.outside_chill_C = ((float) BSWAP16(packet_buffer->loop2_packet.WindChill) - 32.0) / 1.8;
    
    /* Dew point is in fahrenheit. Convert it to Celcius */
    weather_data.dew_point_F = BSWAP16(packet_buffer->loop2_packet.DewPoint);
    weather_data.dew_point_C = ((float) BSWAP16(packet_buffer->loop2_packet.DewPoint) - 32.0) / 1.8;

    /* Inside temperature is in 10th of Fahrenheit. Convert it to Celcius */
    weather_data.inside_temperature_F = (float) BSWAP16(packet_buffer->loop2_packet.InsideTemp) / 10.0;
    weather_data.inside_temperature_C = (weather_data.inside_temperature_F - 32.0) / 1.8;

    /* Inside Humidity is in percent */
    weather_data.inside_humidity = packet_buffer->loop2_packet.InsideHumidity;

    /* Barometric pressure is in inches. Convert to mBar */
    weather_data.barometric_pressure_I = BSWAP16(packet_buffer->loop2_packet.Barometer);
    weather_data.barometric_pressure_Hpa = (33.8639 * weather_data.barometric_pressure_I) / 1000.0;

    /* Rain rate is in rain clicks (TODO : retrieve customized rain sensor value ?) */
    weather_data.rain_rate_I = 0.01 * BSWAP16(packet_buffer->loop2_packet.RainRate);
    weather_data.rain_rate_MM = 0.2 * BSWAP16(packet_buffer->loop2_packet.RainRate);

    /* Day rain is in rain clicks */
    weather_data.rain_day_I = 0.01 * BSWAP16(packet_buffer->loop2_packet.DayRain);
    weather_data.rain_day_MM = 0.2 * BSWAP16(packet_buffer->loop2_packet.DayRain);

    /* Wind speed is in Miles per hour */
    weather_data.wind_speed_MPH = packet_buffer->loop2_packet.WindSpeed;
    weather_data.wind_speed_KPH = 1.609344 * weather_data.wind_speed_MPH;

    /* Wind speed average is in 0.1mph */
    weather_data.wind_speed_avg_2m_MPH = 0.1 * BSWAP16(packet_buffer->loop2_packet.TwoMinuteAvgWindSpeed);
    weather_data.wind_speed_avg_2m_KPH = 1.609344 * weather_data.wind_speed_avg_2m_MPH;

    weather_data.wind_direction = BSWAP16(packet_buffer->loop2_packet.WindDir);

    /* Wind gust is in 0.1mph */
    weather_data.wind_gust = 0.1 * BSWAP16(packet_buffer->loop2_packet.TenMinuteWindGust);
    weather_data.wind_gust_10m = 0.1 * BSWAP16(packet_buffer->loop2_packet.TenMinuteWindGust);

    weather_data.wind_direction_gust_10m = BSWAP16(packet_buffer->loop2_packet.WindDirForTenMinuteWindGust);
  }
  else
  {
    LOG_printf(LOG_LVL_ERROR, "Unknown packet type %d\n", packet_buffer->loop_packet.packet_type);
    return;
  }

  if (vantage_console_priv->DataReadyIndicateCb != NULL)
  {
    vantage_console_priv->DataReadyIndicateCb(&weather_data);
  }
  
}

static void console_process_data(vantage_console_context_t *vantage_console_priv,
                            unsigned char* buf, int size)
{
  unsigned char *pbuf = buf;

  while(size--)
  {
    if (loglevel > 2)
    {
      printf("console_process_data : State %d, pbuf = 0x%02x, rcvd %d\n", vantage_console_priv->decoder_state, *pbuf, vantage_console_priv->data_recvd_length);
    }

    switch (vantage_console_priv->decoder_state)
    {
      case VTG_DECODER_STATE_WAIT_LOOP_PARTIAL_HEADER:
        /* Wait for "OO" characters */
        if (*pbuf == 'O')
        {
          vantage_console_priv->decoder_message_buffer[vantage_console_priv->data_recvd_length] = * pbuf;
          vantage_console_priv->data_recvd_length ++;
          if (vantage_console_priv->data_recvd_length == 3)
          {
            vantage_console_priv->decoder_state = VTG_DECODER_STATE_WAIT_LOOP_PACKET_DATA_END;
          }
          break;
        }
        else
        {
          vantage_console_priv->decoder_state = VTG_DECODER_STATE_WAIT_ANY;
        }
        /* No break so that if the received character is a 'L', we try to decode as a new frame */

      case VTG_DECODER_STATE_WAIT_ANY:
        /* If we received a 'L', then we are receiveing a Loop message */
        if (*pbuf == 'L')
        {
          vantage_console_priv->decoder_message_buffer[0] = *pbuf;
          vantage_console_priv->decoder_state = VTG_DECODER_STATE_WAIT_LOOP_PARTIAL_HEADER;
          vantage_console_priv->data_recvd_length = 1;
          vantage_console_priv->message_awaited_length = LOOP_PACKET_SIZE;
        }

      break;

      case VTG_DECODER_STATE_WAIT_LOOP_PACKET_DATA_END:
        vantage_console_priv->decoder_message_buffer[vantage_console_priv->data_recvd_length] = * pbuf;
        vantage_console_priv->data_recvd_length ++;
        if (vantage_console_priv->message_awaited_length == vantage_console_priv->data_recvd_length)
        {
          if (console_check_crc(vantage_console_priv->decoder_message_buffer,
                                vantage_console_priv->data_recvd_length) == 1)
          {
            console_process_loop_message(vantage_console_priv);
          }
          else
          {
            LOG_printf(LOG_LVL_ERROR, "CRC Error in LOOP message\n");
          }

          vantage_console_priv->decoder_state = VTG_DECODER_STATE_WAIT_ANY;
          vantage_console_priv->data_recvd_length = 0;
          vantage_console_priv->message_awaited_length = 0;
        }
      break;

      
    }

    pbuf ++;

  }
}

static void* console_reader_thread(void *arg)
{
  vantage_console_context_t *vantage_console_priv = (vantage_console_context_t *)arg;
 
  unsigned char in_buf[TTY_RECV_BUFFER_SIZE];
  int n;
  uint64_t exp;
  int timeout_sec = (VANTAGE_PERIODIC_WEATHER_DATA_QUERY_IN_S + 1);

  struct pollfd fds[2];
  int     console_tty_fd = vantage_console_priv->console_tty_fd;
  int     timer_fd = vantage_console_priv->timer_fd;
  int     ret, console_need_reopen;

  if (loglevel > 0)
  {
    LOG_printf(LOG_LVL_INFO, "TTY Reader thread started\n");
  }

  /* Prepare the poll fd set */
  fds[0].fd = console_tty_fd;
  fds[0].events = POLLIN | POLLERR | POLLHUP | POLLNVAL;
  fds[1].fd = timer_fd;
  fds[1].events = POLLIN;

  while(vantage_console_priv->tty_thread_stop_req == 0)
  {
    console_need_reopen = 0;

    if (loglevel > 2)
    {
      LOG_printf(LOG_LVL_DEBUG, "TTY thread -> select\n");
    }

    ret = poll(fds, 2, timeout_sec * 1000);
    if (ret == -1) 
    {
      LOG_printf(LOG_LVL_ERROR, "Poll error. Errno %d\n", errno);
      return NULL;
    }

    if (loglevel > 2)
    {
      LOG_printf(LOG_LVL_DEBUG, "TTY thread poll returns %d\n", ret);
    }

    if (ret == 0)
    {
      LOG_printf(LOG_LVL_ERROR, "Poll timeout. Try to re-open the tty. Errno %d\n", errno);

      console_need_reopen = 1;
      goto console_reopen_check;
    }
    else
    {
      if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
      {
        LOG_printf(LOG_LVL_ERROR, "Error detected on the TTY device. Try to re-open the tty. revents %x\n", fds[0].revents);

        console_need_reopen = 1;
        goto console_reopen_check;
      }
      else if (fds[0].revents & POLLIN)
      {
        if (loglevel > 2)
        {
          LOG_printf(LOG_LVL_DEBUG, "Console FD set\n");
        }
        n = read(console_tty_fd, in_buf, TTY_RECV_BUFFER_SIZE);
        if (n > 0) {
          if (loglevel > 2)
          {
            LOG_printf(LOG_LVL_DEBUG, "Readed %d bytes from TTY\n", n);
          }
          console_process_data(vantage_console_priv, in_buf, n);
        }
        else
        {
          LOG_printf(LOG_LVL_ERROR, "0 bytes read from the TTY device. Try to re-open the tty. revents %x\n");

          console_need_reopen = 1;
          goto console_reopen_check;
        }

        timeout_sec = (VANTAGE_PERIODIC_WEATHER_DATA_QUERY_IN_S + 1);
      }

      if (fds[1].revents & POLLIN)
      {
        if (loglevel > 2)
        {
          LOG_printf(LOG_LVL_DEBUG, "Timer FD set\n");
        }
        read(timer_fd, &exp, sizeof(uint64_t));

        if (loglevel > 0)
        {
          LOG_printf(LOG_LVL_INFO, "Send LOOP%s packet request\n", use_loop2 ? "2" : "");
        }

        /* Wake-up console before LOOP packet request */
        console_wakeup(console_tty_fd);

        if (use_loop2)
        {
          write(console_tty_fd, LOOP2_PACKET_REQ, sizeof(LOOP2_PACKET_REQ));  /* Request LOOP2 packet */
        }
        else
        {
          write(console_tty_fd, LOOP_PACKET_REQ, sizeof(LOOP_PACKET_REQ));  /* Request LOOP packet */
        }

        timeout_sec = 2;
      }
    }

console_reopen_check:
    if (console_need_reopen)
    {
      do
      {
        close(console_tty_fd);
        vantage_console_priv->console_tty_fd = -1;

        console_tty_fd = console_tty_probe(vantage_console_priv->console_dev_path, vantage_console_priv->use_usb_serial);
        if (console_tty_fd < 0)
        {
          LOG_printf(LOG_LVL_ERROR, "Could not find console. Try again in 1 second\n");
          sleep(1);
        }
        else
        {
          vantage_console_priv->console_tty_fd = console_tty_fd; 

          timeout_sec = (VANTAGE_PERIODIC_WEATHER_DATA_QUERY_IN_S + 1);

          console_need_reopen = 0;
        }
      } while(console_need_reopen);
    }
  }

  if (loglevel > 0)
  {
    LOG_printf(LOG_LVL_INFO, "TTY Reader thread stopped\n");
  }

  return NULL;
}

static void console_reader_thread_stop(vantage_console_context_t *vantage_console_priv)
{
  if (loglevel > 0)
  {
    LOG_printf(LOG_LVL_INFO, "TTY Reader thread stop request\n");
  }

  vantage_console_priv->tty_thread_stop_req = 1;
}

/*******************************************************************************
 * Public functions
 ******************************************************************************/
int VTG_console_init(char* dev_path, int use_usb_serial, VTG_DataReadyIndicateCb_t DataReadyIndicateCb)
{
  struct itimerspec timer_new_value;

  vantage_console_priv.use_usb_serial = use_usb_serial;
  strncpy(vantage_console_priv.console_dev_path, dev_path, MAX_DEV_PATH_LEN);

  int tty_fd = console_tty_probe(dev_path, use_usb_serial);
  if (tty_fd < 0)
  {
    return -1;
  }

  vantage_console_priv.console_tty_fd = tty_fd;
  vantage_console_priv.DataReadyIndicateCb = DataReadyIndicateCb;


  console_wakeup(vantage_console_priv.console_tty_fd);

  vantage_console_priv.timer_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
  if (vantage_console_priv.timer_fd == -1)
  {
    LOG_printf(LOG_LVL_ERROR, "Error creating timer fd\n");
    return -1;
  }

  vantage_console_priv.tty_thread_stop_req = 0;
  pthread_create(&(vantage_console_priv.tty_reader_thread), NULL, 
                 &console_reader_thread, &vantage_console_priv);

  timer_new_value.it_interval.tv_sec = VANTAGE_PERIODIC_WEATHER_DATA_QUERY_IN_S;
  timer_new_value.it_interval.tv_nsec = 0;
  timer_new_value.it_value.tv_sec = 1;
  timer_new_value.it_value.tv_nsec = 0;

  if (timerfd_settime(vantage_console_priv.timer_fd, 0, &timer_new_value, NULL) == -1)
  {
    LOG_printf(LOG_LVL_ERROR, "%s: timerfd_settime error (%d)\n", 
                    __FUNCTION__, errno);
    return -1;
  }

  return 0;
}

void VTG_console_exit(void)
{
  console_reader_thread_stop(&vantage_console_priv);
  pthread_join(vantage_console_priv.tty_reader_thread, NULL);

  console_tty_close(vantage_console_priv.console_tty_fd);
  
}
