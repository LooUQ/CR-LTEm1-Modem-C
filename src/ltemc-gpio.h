/** ***************************************************************************
  @file 
  @brief Modem GPIO/ADC expansion features/services.

  @author Greg Terrell, LooUQ Incorporated

  \loouq
-------------------------------------------------------------------------------

LooUQ-LTEmC // Software driver for the LooUQ LTEm series cellular modems.
Copyright (C) 2022-2023 LooUQ Incorporated

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
Also add information on how to contact you by electronic and paper mail.

**************************************************************************** */


#ifndef __LTEMC_GPIO_H__
#define __LTEMC_GPIO_H__

#include "ltemc.h"


/**
 * @brief GPIO subsystem constants
 */
enum gpio__constants
{
    gpio__BG77__maxPin = 9,
    adc__BG77__maxPin = 1,

    gpio__LTEM3F__maxPin = 6,
    adc__LTEM3F__maxPin = 2,
};


/**
 * @brief Determines the GPIO pin direction: input or output
 */
typedef enum gpioDirection_tag
{
    gpioDirection_input = 0,
    gpioDirection_output = 1
} gpioDirection_t;


/**
 * @brief The type of "pull" to apply to an GPIO input pin
 */
typedef enum gpioPull_tag
{
    gpioPull_none = 0,
    gpioPull_down = 1,
    gpioPull_up = 3,
    gpioPull_noChange = 2,
    gpioPull_NA = 255                                               // added to enum for source intent readability 
} gpioPull_t;


/**
 * @brief If pull mode is set, the amount of drive current to apply (set in 2mA increments).
 * 
 */
typedef enum gpioPullDrive_tag
{
    gpioPullDrive_2mA = 0,
    gpioPullDrive_4mA = 1,
    gpioPullDrive_6mA = 2,
    gpioPullDrive_8mA = 3,
    gpioPullDrive_10mA = 4,
    gpioPullDrive_12mA = 5,
    gpioPullDrive_14mA = 6,
    gpioPullDrive_16mA = 7,
    gpioPullDrive_NA = 255              // added to enum for source intent readability 
} gpioPullDrive_t;


#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Read valule of ADC interface.
 * 
 * @param portNumber The ADC port number (0 or 1) to read.
 * @param analogValue Pointer to location where read value should be stored.
 * @return resultCode_t 
 */
resultCode_t gpio_adcRead(uint8_t portNumber, uint16_t* analogValue);


/**
 *	@brief Configure a GPIO port for intended use.
 *	@param [in] portNumber The GPIO port to configured, dependent on modem module and modem board.
 *	@param [in] direction Set the GPIO port to be for input or output.
 *	@param [in] pullType If the port is input, specifies the optional pull up/down behavior for the GPIO port. Ignored if "direction" is output.
 *	@param [in] pullDriveCurrent If the port is input AND pull is specified, sets the current limit for the pull circuit. Ignored if "direction" is output.
 *  @return Value read 0=low, 1=high. Result of -1 indicates invalid port number.
 */
resultCode_t gpio_configPort(uint8_t portNumber, gpioDirection_t direction, gpioPull_t pullType, gpioPullDrive_t pullDriveCurrent);


/**
 *	@brief Read digitial value from a GPIO port.
 *
 *	@param [in] portNumber The GPIO port to read, dependent on modem module and modem board.
 *	@param [in] pinValue Pointer to location to receive the read GPIO value (0 or 1).
 *  @return Value read 0=low, 1=high. Result of -1 indicates invalid port number.
 */
resultCode_t gpio_read(uint8_t portNumber, bool* pinValue);


/**
 *	@brief Write digital value from to GPIO port.
 *
 *	@param [in] portNumber The GPIO port to read, dependent on modem module and modem board.
 *	@param [in] pinValue The 0 or 1 digital value to assign to the GPIO port output.
 *  @return Error status: 0=no error, -1 indicates invalid port number.
 */
resultCode_t gpio_write(uint8_t portNumber, bool pinValue);


#ifdef __cplusplus
}
#endif

#endif  // !__LTEMC_GPIO_H__
