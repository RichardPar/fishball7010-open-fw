#!/bin/sh
#https://www.analog.com/media/cn/technical-documentation/user-guides/AD9364_Register_Map_Reference_Manual_UG-672.pdf

adphys="$(cat /sys/bus/iio/devices/iio:device0/name)"
tx_power_save=$(fw_printenv -n tx_power_save)


ptton()
{
    #PTT on GPIO 0  (GPIO 1 should be not touched)
        # Get the current register value
        if [ "$adphys" = "ad9361-phy" ] ; then

                echo 0x27 > /sys/kernel/debug/iio/iio:device0/direct_reg_access
                CURRENT_VALUE=$(cat  /sys/kernel/debug/iio/iio:device0/direct_reg_access | grep -o '0x[0-9a-fA-F]\+')
                if [ "$tx_power_save" = "yes" ] ; then
                        echo 0 > /sys/bus/iio/devices/iio:device0/out_altvoltage1_TX_LO_powerdown  
                fi
        else

                echo 0x27 > /sys/kernel/debug/iio/iio:device1/direct_reg_access
                CURRENT_VALUE=$(cat  /sys/kernel/debug/iio/iio:device1/direct_reg_access | grep -o '0x[0-9a-fA-F]\+')
                if [ "$tx_power_save" = "yes" ] ; then
                        echo 0 > /sys/bus/iio/devices/iio:device0/out_altvoltage1_TX_LO_powerdown  
                fi
        fi
        
        if [ -z "$CURRENT_VALUE" ]; then
                echo "Failed to retrieve the current register value." >> /tmp/watch.txt
                exit 1
        fi

        # Convert the current value from hex to decimal
        CURRENT_VALUE_DEC=$((CURRENT_VALUE))

        # Clear the bits corresponding to the PARAM_VALUE before setting the new value
        CLEAR_MASK=$((~0x10))
        CURRENT_VALUE_CLEARED=$((CURRENT_VALUE_DEC & CLEAR_MASK))

        # Perform bitwise OR with the parameter value
        RESULT_VALUE=$((CURRENT_VALUE_CLEARED |0x10))

        # Convert the result back to hex
        RESULT_VALUE_HEX=$(printf "0x%X" $RESULT_VALUE)

        # Pass the result to the direct register access, keeping the address 0x27

        #echo "0x27 $RESULT_VALUE_HEX" > /sys/kernel/debug/iio/iio:device0/direct_reg_access
    if [ "$adphys" = "ad9361-phy" ] ; then
                echo "0x27 $RESULT_VALUE_HEX" > /sys/kernel/debug/iio/iio:device0/direct_reg_access
    else
                echo "0x27 $RESULT_VALUE_HEX" > /sys/kernel/debug/iio/iio:device1/direct_reg_access
    fi

    #echo 1 > /sys/class/gpio/gpio906/value
    #gpioset  gpiochip0 80=1
    echo 1 > /sys/class/leds/ptt/brightness
    echo "$(date) watchconsoleTX PTT_ON" >> /tmp/lnb.txt    



}

pttoff()
{
    # Get the current register value
               if [ "$adphys" = "ad9361-phy" ] ; then

                echo 0x27 > /sys/kernel/debug/iio/iio:device0/direct_reg_access
                CURRENT_VALUE=$(cat  /sys/kernel/debug/iio/iio:device0/direct_reg_access | grep -o '0x[0-9a-fA-F]\+')
                if [ "$tx_power_save" = "yes" ] ; then
                        echo 1 > /sys/bus/iio/devices/iio:device0/out_altvoltage1_TX_LO_powerdown  
                fi
                
        else

                echo 0x27 > /sys/kernel/debug/iio/iio:device1/direct_reg_access
                CURRENT_VALUE=$(cat  /sys/kernel/debug/iio/iio:device1/direct_reg_access | grep -o '0x[0-9a-fA-F]\+')
                if [ "$tx_power_save" = "yes" ] ; then
                        echo 1 > /sys/bus/iio/devices/iio:device0/out_altvoltage1_TX_LO_powerdown  
                fi
                
        fi


        if [ -z "$CURRENT_VALUE" ]; then
                 echo "Failed to retrieve the current register value." >> /tmp/watch.txt
                exit 1
        fi

        # Convert the current value from hex to decimal
        CURRENT_VALUE_DEC=$((CURRENT_VALUE))

        # Clear the bits corresponding to the PARAM_VALUE before setting the new value
        CLEAR_MASK=$((~0x10))
        CURRENT_VALUE_CLEARED=$((CURRENT_VALUE_DEC & CLEAR_MASK))

        # Perform bitwise OR with the parameter value
        RESULT_VALUE=$((CURRENT_VALUE_CLEARED |0x00))

        # Convert the result back to hex
        RESULT_VALUE_HEX=$(printf "0x%X" $RESULT_VALUE)

        # Pass the result to the direct register access, keeping the address 0x27
        #echo "0x27 $RESULT_VALUE_HEX" > /sys/kernel/debug/iio/iio:device0/direct_reg_access


    if [ "$adphys" = "ad9361-phy" ] ; then
            echo "0x27 $RESULT_VALUE_HEX" > /sys/kernel/debug/iio/iio:device0/direct_reg_access
    else
        echo "0x27 $RESULT_VALUE_HEX" > /sys/kernel/debug/iio/iio:device1/direct_reg_access
    fi
    #echo 0 > /sys/class/gpio/gpio906/value
    #gpioset  gpiochip0 80=0
    echo 0 > /sys/class/leds/ptt/brightness
    echo "$(date) watchconsoleTX PTT_OFF" >> /tmp/lnb.txt
}


if [ "$adphys" = "ad9361-phy" ] ; then
        echo manual_tx_quad > /sys/bus/iio/devices/iio:device0/calib_mode
        #Manual GPIO
        echo 0x26 0x10 > /sys/kernel/debug/iio/iio:device0/direct_reg_access
else
        echo manual_tx_quad > /sys/bus/iio/devices/iio:device1/calib_mode
        #Manual GPIO
        echo 0x26 0x10 > /sys/kernel/debug/iio/iio:device1/direct_reg_access
fi

#MIO for plutoplus : MIO start at 906, EMIO at 960
#echo "906" > /sys/class/gpio/export
#echo out > /sys/class/gpio/gpio906/direction

pttoff

# CPU: this loop runs every second, so it avoids forking. The interrupt count is read with shell builtins
# (same value as: grep "7c42" /proc/interrupts | awk '{print $2 + $3}'), and the PTT register / LED / log
# are only touched when the wanted state changes or something else moved the PTT LED away from it.
read_tx_irq() {
    CURRENT_COUNT=""
    while read -r _l; do
        case "$_l" in
            *7c42*) set -- $_l; CURRENT_COUNT=$(( $2 + $3 )); break ;;
        esac
    done < /proc/interrupts
}

# Initialize the history variable before starting the loop
read_tx_irq
LAST_COUNT=$CURRENT_COUNT
LAST_STATE=""

check_tx_flux() {
  # Get the current absolute count immediately
    read_tx_irq

    # Compare it directly to the previous loop's count
    DELTA=$((CURRENT_COUNT - LAST_COUNT))

    # Save the current count as history for the NEXT loop execution
    LAST_COUNT=$CURRENT_COUNT

    if [ "$DELTA" -gt 0 ]; then WANT=1; else WANT=0; fi

    PTT_NOW=""
    read -r PTT_NOW < /sys/class/leds/ptt/brightness 2>/dev/null
    if [ "$PTT_NOW" != "0" ]; then PTT_NOW=1; fi

    if [ "$WANT" != "$LAST_STATE" ] || [ "$PTT_NOW" != "$WANT" ]; then
        if [ "$WANT" = 1 ]; then
            ptton
        else
            pttoff
        fi
        LAST_STATE=$WANT
    fi
}


loop()
{


while :
do
        check_tx_flux
        sleep 1
done
}

loop
