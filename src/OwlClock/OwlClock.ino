

#include <Adafruit_NeoPixel.h> // https://github.com/adafruit/Adafruit_NeoPixel
#include <DS3232RTC.h>         // http://github.com/JChristensen/DS3232RTC
#include <Time.h>              // http://www.arduino.cc/playground/Code/Time
#include <Wire.h>              // http://arduino.cc/en/Reference/Wire (included with Arduino IDE)
#include <EEPROM.h>
#include "Declarations.h"

// My helper lib that has moon and holiday code in it.
extern "C" {
#include "AstronomicalCalculations.h"
}

#define LIGHT_SENSOR_TOP_ANALOG_IN     A0
#define LIGHT_SENSOR_INSIDE_ANALOG_IN  A1
#define LED_COUNT 40

#define LED_DIG_OUT        6

#define NOT_RUNNING_ON_UNO

#ifdef NOT_RUNNING_ON_UNO
    #define DEBUG_PRINT(x)    Serial.print(x)
    #define DEBUG_PRINTLN(x)  Serial.println(x)
#else
    #define DEBUG_PRINT(x)    {}  
    #define DEBUG_PRINTLN(x ) {} 
#endif

#ifdef RUNNING_ON_UNO
    #define SWITCH_UP_DIG_IN   2
    #define SWITCH_DOWN_DIG_IN 3
    #define SWITCH_SET_DIG_IN  4
#else
// This has up/down reversed from what is silscreened on the boards, but this
// is the "right" way around in term of feel.
    #define SWITCH_UP_DIG_IN   1
    #define SWITCH_DOWN_DIG_IN 0
    #define SWITCH_SET_DIG_IN  2
#endif

// Neo pixel interface for all the LEDS is though the single pin LED_DIG_OUT
Adafruit_NeoPixel leds = Adafruit_NeoPixel(LED_COUNT, LED_DIG_OUT, NEO_GRB + NEO_KHZ800);

// digit to LED index for the clock digits. Tens and Ones are sequenced differently.
static const uint8_t gTensDigit[] = {6, 9, 4, 3, 1, 8, 5, 2, 0, 7};
static const uint8_t gOnesDigit[] = {5, 0, 3, 6, 8, 1, 4, 7, 9, 2};

EEPROMInfo gInfo;
uint32_t gHourTensColor = 0xff0a0a;
uint32_t gHourOnesColor = 0xff0a0a;
uint32_t gMinTensColor  = 0xff0a0a;
uint32_t gMinOnesColor  = 0xff0a0a;

uint32_t gHourTensScale = 0xff;
uint32_t gHourOnesScale = 0xff;
uint32_t gMinTensScale  = 0xff;
uint32_t gMinOnesScale  = 0xff;


#define DEFAULT_GMT_OFFSET -8*60*60;

// To addess each digit use these bases + the above index.
#define HOUR_TENS_BASE    20
#define HOUR_ONES_BASE    30
#define MINUTE_TENS_BASE   0
#define MINUTE_ONES_BASE  10
#define NO_INDEX 255

// The various moon arcs, have digit equivalents, so we list those here.
// The percentages go from upper right to lower left.
#define ZERO_PERCENT_MOON_ARC 20
#define TWENTY_PERCENT_MOON_ARC 25
#define FORTY_PERCENT_MOON_ARC 21
#define SIXTY_PERCENT_MOON_ARC 23
#define EIGHTY_PERCENT_MOON_ARC 28
#define ONE_HUNDRED_PERCENT_MOON_ARC 22

// The other special characters.
#define SUN_OUTLINE 26
#define EYE_OUTLINE 27

#define EYE_SEAK_DURATION 150

#define EEPROM_INFO_ADDRESS 0
#define EEPROM_NEVER_WAS_SET 255
#define EEPROM_INFO_VERSION 1   // Increment this if you change/reorder the EEPROMInfo


#define DEFAULT_LATITUDE 36.3894     // These are burned into the EEPROM on starup/version bump.
#define DEFAULT_LONGITUDE -122.0819

// I wrote a ton of code to compute these sun events, but the code took 24k of program space,
// so instead I'm squeezing the next 100 years of sun events into these arrays.  The
// value are two bit offsets from their base value.  So 4 years per byte. That makes it
// simple to do the progmem lookup for the correct byte and mask from there.

const uint8_t springEquinoxArray[] PROGMEM = { 85, 85, 85, 85, 85, 85, 85, 84, 84, 84, 84, 84,
                                               84, 84, 84, 80, 80, 80, 80, 80, 80, 165, 165, 149, 149, };
const uint8_t summerSolsticeArray[] PROGMEM = { 84, 84, 84, 84, 84, 84, 80, 80, 80, 80, 80, 80, 
                                                80, 64, 64, 64, 64, 64, 64, 64, 0, 85, 85, 85, 85, };
const uint8_t fallEquinoxArray[] PROGMEM = { 165, 165, 165, 149, 149, 149, 149, 149, 149, 149, 149, 85, 85, 
                                             85, 85, 85, 85, 85, 85, 84, 84, 169, 169, 169, 169, };
const uint8_t winterSolsticeArray[] PROGMEM = { 149, 149, 149, 149, 149, 149, 149, 85, 85, 85, 85, 85, 85, 85, 
                                                85, 85, 84, 84, 84, 84, 84, 169, 169, 169, 165, };

#define SPRING_EQUINOX_DAY_BASE 19
#define SUMMER_SOLSTICE_DAY_BASE 20
#define FALL_EQUINOX_DAY_BASE 21
#define WINTER_SOLSTICE_DAY_BASE 20


#define SUN_EVENT_YEAR_BASE 2016
#define SUN_EVENT_YEAR_RANGE 100

// This should be passed one of the PROGMEM solar event arrays above with the matching DAY_BASE
uint8_t unpackSunEventDay(uint8_t *pgmArray, uint8_t dayBase, int year)
{
  // If the year is in the past, something has gone wrong, and we don't 
  // care the the solar events are wrong.
  if (year < SUN_EVENT_YEAR_BASE) {
    year = SUN_EVENT_YEAR_BASE;
  }
  
  int index = (year - SUN_EVENT_YEAR_BASE)/4;
  int shift = ((year - SUN_EVENT_YEAR_BASE)%4)*2;

  // To keep things from crashing 100 years from now we loop around
  // after that time the sun events will be incorrect, but all the people
  // I have given the clocks to will be dead so no one will care.
  index = index%SUN_EVENT_YEAR_RANGE;

  uint8_t mask = 3 << shift;
  
  return ((pgm_read_byte(&pgmArray[index]) & mask) >> shift) + dayBase;
}

// This looks up a display digit based on the base and the ordering arrays.
uint8_t ledIndex(uint8_t base, uint8_t digit)
{
    // Possibly make this do PROGMEM lookups
    if (base == MINUTE_ONES_BASE || base == HOUR_ONES_BASE) {
        return base + gOnesDigit[digit];
    }
    return base + gTensDigit[digit];
}

void setup(void)
{
    // Add pullup resistors to all the switch inputs.
    pinMode(SWITCH_UP_DIG_IN, INPUT_PULLUP);
    pinMode(SWITCH_DOWN_DIG_IN, INPUT_PULLUP);
    pinMode(SWITCH_SET_DIG_IN, INPUT_PULLUP);
  

  //setSyncProvider() causes the Time library to synchronize with the
  //external RTC by calling RTC.get() every five minutes by default.
  setSyncProvider(RTC.get);

#ifdef RUNNING_ON_UNO
   Serial.begin(19200);
   if (timeStatus() != timeSet) {
     Serial.print(F("RTC Sync setup failed."));
   }
#endif
  readAndValidateEEPromInfo();
 
  leds.begin();
  clearLeds();
  leds.show();
}

void
readAndValidateEEPromInfo()
{
    EEPROM.get(EEPROM_INFO_ADDRESS, gInfo);

    // If we have never saved this info off to eeprom
    // we have to assume the current clock is ok and recompute everything based on that.
    if (gInfo.version != EEPROM_INFO_VERSION) {
        gInfo.gmtOffsetInSeconds = DEFAULT_GMT_OFFSET;
        time_t t = now() + gInfo.gmtOffsetInSeconds;        
        writeEEProm(t);
    }

    //        bool oscStopped(bool clearOSF = true);  //defaults to clear the OSF bit if argument not supplied
    // TODO: Maybe I should add some code that blinks if the oscStopped flag has been set on reboot.
}

void
writeEEProm(time_t t)
{
    gInfo.version = EEPROM_INFO_VERSION;
    gInfo.year = (unsigned short)year(t);
    gInfo.month = month(t);
    gInfo.day = day(t);

    EEPROM.put(EEPROM_INFO_VERSION, gInfo);
}

//void computeHolidays(EEPROMInfo *info)
//{

//  int computedYear = 0;
//  int computedMonth = 0;
//  float computedDay = 0.0;

// These pull in too much code to compute dynamically.
// 
//  computeDateFromSolarEventJDE(springEquinox(info->year), &computedYear, &computedMonth, &computedDay);
//  info->springEquinoxDay = floor(computedDay);

//  computeDateFromSolarEventJDE(summerSolstice(info->year), &computedYear, &computedMonth, &computedDay);
//  info->summerSolsticeDay = floor(computedDay); 

//  computeDateFromSolarEventJDE(fallEquinox(info->year), &computedYear, &computedMonth, &computedDay);
//  info->fallEquinoxDay = floor(computedDay); 

//  computeDateFromSolarEventJDE(winterSolstice(info->year), &computedYear, &computedMonth, &computedDay);
//  info->winterSolsticeDay = floor(computedDay); 
  
//  easterForYear(info->year, &(info->easterMonth), &(info->easterDay));

//  info->mothersDay = computeHolidayBasedOnDayOfWeek(info->year, MAY, SUNDAY, 2);
//  info->fathersDay = computeHolidayBasedOnDayOfWeek(info->year, JUNE, SUNDAY, 3);
//  info->presidentsDay = computeHolidayBasedOnDayOfWeek(info->year, FEBRUARY, MONDAY, 3);
//}

int 
twelveHourValueFrom24HourTime(int hour)
{
    while (hour > 12) {
        hour -= 12;
    }
    if (hour == 0) {
        hour = 12;
    }
    return hour;
}

void 
setLedsWithTime(int hours, int minutes)
{
  if (minutes > 59) {
    minutes = 59;
  }

  hours = twelveHourValueFrom24HourTime(hours);

  int hourTens = floor(hours/10);
  int hourOnes = hours - 10*hourTens;
  
  if (hourTens > 0) {
      leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[hourTens], scaleColor(gHourTensColor, gHourTensScale));
  }
  leds.setPixelColor(HOUR_ONES_BASE + gOnesDigit[hourOnes], scaleColor(gHourOnesColor, gHourOnesScale));

  int minuteTens = floor(minutes/10);
  int minuteOnes = minutes - minuteTens*10;

  leds.setPixelColor(MINUTE_TENS_BASE + gTensDigit[minuteTens], scaleColor(gMinTensColor, gMinTensScale));
  leds.setPixelColor(MINUTE_ONES_BASE + gOnesDigit[minuteOnes], scaleColor(gMinOnesColor, gMinOnesScale));
}


void 
displayCountdown(int minutes, int seconds)
{
    while (minutes > 0 && seconds > 0) {
        seconds--;
        if (seconds < 0) {
            seconds = 59;
            minutes--;
        }

        clearLeds();
        setLedsWithTime(minutes, seconds);
        leds.show();
        delay(1000);
    }
}


void
setMoonPhaseLeds(int year, int month, int day, int hour)
{
    float fday = (float)day + (float)hour/24.0;
    float percentIlluminated =  illuminatedFractionKForDate(year, month, fday);
    boolean isWaxing = isWaxingForDate(year, month, fday);
    
    // If the moon is going to over lap with a digit we show it very dimmly.
    hour = twelveHourValueFrom24HourTime(hour);
    uint32_t moonColor = (hour <= 9) ? leds.Color(255, 248, 209) : leds.Color(15,18,13);

    // We use a .05 on either side to make new/full a bit more discriminating.  We might want to
    // make it even smaller.
    if (isWaxing) {
        if (percentIlluminated < 0.01) {
            // New moon, we're done.
        } else {
            // Turn on the upper right rim.
            leds.setPixelColor(ZERO_PERCENT_MOON_ARC, moonColor);

            if (percentIlluminated < 0.05) {
                // For very low percentages we just show a one arc sliver of moon                
            } else if (percentIlluminated < 0.3) {
                leds.setPixelColor(TWENTY_PERCENT_MOON_ARC, moonColor);
            } else if (percentIlluminated < 0.5) {
                leds.setPixelColor(FORTY_PERCENT_MOON_ARC, moonColor);
            } else if (percentIlluminated < 0.7) {
                    leds.setPixelColor(SIXTY_PERCENT_MOON_ARC, moonColor);
            } else if (percentIlluminated < 0.95) {
                leds.setPixelColor(EIGHTY_PERCENT_MOON_ARC, moonColor);
            } else {
                leds.setPixelColor(ONE_HUNDRED_PERCENT_MOON_ARC, moonColor);
            }
        }
    } else {
       // Use the lower left rim.
        if (percentIlluminated < 0.01) {
            // New moon, we're done.
        } else {
            // Turn on the lower left rim.
            leds.setPixelColor(ONE_HUNDRED_PERCENT_MOON_ARC, moonColor);

            if (percentIlluminated < 0.05) {
                // For very low percentages we just show a one arc sliver of moon
            } else if (percentIlluminated < 0.3) {
                leds.setPixelColor(EIGHTY_PERCENT_MOON_ARC, moonColor);
            } else if (percentIlluminated < 0.5) {
                leds.setPixelColor(SIXTY_PERCENT_MOON_ARC, moonColor);
            } else if (percentIlluminated < 0.7) {
                leds.setPixelColor(FORTY_PERCENT_MOON_ARC, moonColor);
            } else if (percentIlluminated < 0.95) {
                leds.setPixelColor(TWENTY_PERCENT_MOON_ARC, moonColor);
            } else {
                leds.setPixelColor(ZERO_PERCENT_MOON_ARC, moonColor);
            }
        }
    }
}


// This info is shared across multiple indepedent buttons with the
// assumption that only one button at a time can be sending auto repeat events.
// So for example using this holding down both up and down buttons would press/autorepeat/release only
// the first of the buttons pressed, not both.
void clearSharedEventState(SharedEventState *eventState)
{
    eventState->buttonPressTime = 0;
    eventState->mostRecentAutoRepeatTime = 0;
    eventState->autoRepeatCount = 0;
    eventState->autoRepeatInterval = MAX_AUTO_REPEAT_INTERVAL;
}

void
initializeButtonInfo(ButtonInfo *buttonInfo, uint8_t pin, uint8_t canAutoRepeat)
{
    buttonInfo->state = 0;
    if (digitalRead(pin)) {
        buttonInfo->state |= BUTTON_STATE_MOST_RECENT_STATE;
    }
    if (canAutoRepeat) {
        buttonInfo->state |= BUTTON_STATE_CAN_AUTO_REPEAT;
    }
    buttonInfo->pin = pin;
}

uint8_t updateButtonInfoWithCurrentState(time_t currentTime,
                                         SharedEventState *eventState,
                                         ButtonInfo *buttonInfo)
{
    bool currentState = digitalRead(buttonInfo->pin);
    //DEBUG_PRINT(F("update "));
    //DEBUG_PRINT(buttonInfo->pin);
    //DEBUG_PRINT(F(" currentState: "));
    //DEBUG_PRINTLN(currentState);
    //DEBUG_PRINT(F("eventState buttonPressTime: "));
    //DEBUG_PRINTLN(eventState->buttonPressTime);
    // If the state is changing then we react to the event.
    if (currentState != ((buttonInfo->state & BUTTON_STATE_MOST_RECENT_STATE) != 0)) {
        DEBUG_PRINT(F(" state changed for pin "));
        DEBUG_PRINTLN(buttonInfo->pin);
      
        // Button is released.
        if (currentState) {
            DEBUG_PRINTLN(F("  released"));
            buttonInfo->state |= BUTTON_STATE_MOST_RECENT_STATE;
            // If we owned the press time we clear our hold and return a release.
            if (buttonInfo->state & BUTTON_STATE_OWNS_START_TIME) {
                DEBUG_PRINTLN(F("  we owned it, clearing state"));
                clearSharedEventState(eventState);
                buttonInfo->state &= ~BUTTON_STATE_OWNS_START_TIME;

                // Only send the release if we sent a press.  Otherwise this was noise.
                if (buttonInfo->state & BUTTON_STATE_OFFICALLY_PRESSED) {
                    buttonInfo->state &= ~BUTTON_STATE_OFFICALLY_PRESSED;
                    DEBUG_PRINT(F("OFFICALLY RELEASED PIN: "));
                    DEBUG_PRINTLN(buttonInfo->pin);
                    return BUTTON_RELEASED;
                }
            } else {
                DEBUG_PRINTLN(F("  did not own"));
            }
        } else {
            DEBUG_PRINTLN(F("  pressed"));
            // Button is pressed.
            buttonInfo->state &= ~BUTTON_STATE_MOST_RECENT_STATE;

            // If no one owns the event state we grab it to lock out other buttons.
            if (eventState->buttonPressTime == 0) {
                DEBUG_PRINTLN(F("  Not owned, grabbing state"));
                eventState->buttonPressTime = currentTime;
                eventState->mostRecentAutoRepeatTime = 0;
                buttonInfo->state |= BUTTON_STATE_OWNS_START_TIME;
            }
        }
        
        // State changed, but we don't have an offical event, so we
        // just return no event. Presses are delayed to supress bounce.
        return BUTTON_NO_EVENT;
    }
    
    
    if (buttonInfo->state & BUTTON_STATE_OWNS_START_TIME) {
        
        // If we haven't offically sent out the press we do that.
        if (!(buttonInfo->state & BUTTON_STATE_OFFICALLY_PRESSED)) {
            DEBUG_PRINTLN(F("Not offically pressed, checking for long enough delay"));
            if (currentTime - eventState->buttonPressTime > BUTTON_DEBOUNCE_INTERVAL ) {
                DEBUG_PRINT(F("Long Enough, OFFICALLY PRESSED PIN: "));
                DEBUG_PRINTLN(buttonInfo->pin);         
                buttonInfo->state |= BUTTON_STATE_OFFICALLY_PRESSED;
                // Set this so we can get the max time until first auto repeat.
                eventState->mostRecentAutoRepeatTime = currentTime;
                return BUTTON_PRESSED;
            }
            return BUTTON_NO_EVENT;
        } else {
            if (buttonInfo->state & BUTTON_STATE_CAN_AUTO_REPEAT) {
                DEBUG_PRINT(buttonInfo->pin);
                DEBUG_PRINT(".");
            } else {
                DEBUG_PRINT(buttonInfo->pin);                
                DEBUG_PRINT("*");
            }
            // If we may need to emit an auto repeat press we check for that.
            if (buttonInfo->state & BUTTON_STATE_CAN_AUTO_REPEAT) {
                if (currentTime - eventState->mostRecentAutoRepeatTime > eventState->autoRepeatInterval) {
                    eventState->mostRecentAutoRepeatTime = currentTime;
                
                    // For a while we step along at the max interval
                    if (eventState->autoRepeatCount < AUTO_REPEAT_RAMP_DELAY) {
                        eventState->autoRepeatInterval = MAX_AUTO_REPEAT_INTERVAL;
                        eventState->autoRepeatCount++;
                        // Then we ramp to the fastest interval
                    } else if (eventState->autoRepeatInterval > MIN_AUTO_REPEAT_INTERVAL) {
                        eventState->autoRepeatInterval -= AUTO_REPEAT_RAMP_STEP;
                    } else {
                        // Then we just hold there forever.
                        eventState->autoRepeatInterval = MIN_AUTO_REPEAT_INTERVAL;
                    }

                    DEBUG_PRINT(F("AUTO REPEATE PIN: "));
                    DEBUG_PRINTLN(buttonInfo->pin);             
                    
                    return BUTTON_AUTO_REPEAT;
                }
            }
        }
    }
    
    return BUTTON_NO_EVENT;
}

void showCurrentMoonAndTime(time_t t)
{
    clearLeds();
    //    flagWaveAnimation();    
    setLedsWithTime(hour(t), minute(t));
    //goldenRingSpin();
    //spinGlobe();
      //        eyeAnimation();
    setMoonPhaseLeds(year(t), month(t), day(t), hour(t));
    leds.show();
}

void checkForDayDisplayModeChange(time_t t)
{
    // If the day has changed, we need to check if this is a day holiday, etc.
    if (gInfo.day == day(t)) {
        return;
    }    
}

void loop()
{
    DEBUG_PRINTLN(F("LOOP"));
    // If the user hits the set button, we go though the setup sequence.
    if (digitalRead(SWITCH_SET_DIG_IN) == false) {
        delay(50);
        performUserSetupSequence();
    }

    time_t t = now() + gInfo.gmtOffsetInSeconds;
    
    //    checkForDayDisplayModeChange(t);
    showCurrentMoonAndTime(t);
}

void clearLeds()
{
    for (int i=0; i<LED_COUNT; i++) {
        leds.setPixelColor(i, 0);
    }
}

void clearSpecialCharacterLeds()
{
    leds.setPixelColor(ledIndex(HOUR_TENS_BASE, 0), 0);
    for (int i=3; i<10; ++i ) {
        leds.setPixelColor(ledIndex(HOUR_TENS_BASE, i), 0);
    }
}

void setAllDigits(uint8_t base, uint32_t color)
{
    if (base == NO_INDEX) {
        return;
    }
    for (uint8_t digit = 0; digit < 10; ++digit) {
        leds.setPixelColor(ledIndex(base, digit), color);
    }
}


void displayValue(int value,
                  uint8_t digitOnesBase,
                  uint8_t digitTensBase,
                  uint8_t digitHundredsBase,
                  uint8_t digitThousandsBase,
                  uint8_t colorScale,
                  uint8_t digitType)
{
    uint32_t color = (value < 0) ? leds.Color(0, 128, 255) : leds.Color(254, 120, 0);
    uint32_t colorBlack = 0;

    clearSpecialCharacterLeds();

    bool isPM = false;
    if (digitType == DIGIT_TYPE_HOURS) {
        if (value >= 12) {
            isPM = true;
        }
        value = twelveHourValueFrom24HourTime(value);
    }

    value = abs(value);
    
    uint8_t onesDigit = value%10;
    value = value/10;
    uint8_t tensDigit = value%10;
    value = value/10;
    uint8_t hundredsDigit = value%10;
    value = value/10;
    uint8_t thousandsDigit = value%10;

    // If we're showing minutes, we always show the leading zeros.
    bool foundNonZeroDigit = digitType == DIGIT_TYPE_MINUTES;

    // Clear any digits we will be writing into.
    setAllDigits(digitOnesBase, 0);
    setAllDigits(digitTensBase, 0);
    setAllDigits(digitHundredsBase, 0);
    setAllDigits(digitThousandsBase, 0);

    // As a "PM Indicator" we display a big circle behind the hour tens digit.
    // In either the "first half of the 24 hour clock face" or seconds half.
    if (digitType == DIGIT_TYPE_HOURS) {
        if (isPM) {
            leds.setPixelColor(ONE_HUNDRED_PERCENT_MOON_ARC, leds.Color(255, 5, 5));
        } else {
            leds.setPixelColor(ZERO_PERCENT_MOON_ARC, leds.Color(255, 5, 5));
        }
    } else if (digitType == DIGIT_TYPE_MONTH) {
        // For months we show a moon
        leds.setPixelColor(ONE_HUNDRED_PERCENT_MOON_ARC, leds.Color(255, 248, 209));
        leds.setPixelColor(EIGHTY_PERCENT_MOON_ARC, leds.Color(255, 248, 209));
    } else if (digitType == DIGIT_TYPE_DAY) {
        // For days we show a sun.
        leds.setPixelColor(ONE_HUNDRED_PERCENT_MOON_ARC, leds.Color(254, 120, 0));
        leds.setPixelColor(ZERO_PERCENT_MOON_ARC, leds.Color(254, 120, 0));
        leds.setPixelColor(SUN_OUTLINE, leds.Color(254, 120, 0));
    }

    color = scaleColor(color, colorScale);
    
    foundNonZeroDigit |= thousandsDigit != 0;
    if (digitThousandsBase != NO_INDEX) {
        leds.setPixelColor(ledIndex(digitThousandsBase, thousandsDigit), (foundNonZeroDigit) ? color : colorBlack);
    }

    foundNonZeroDigit |= hundredsDigit != 0;
    if (digitHundredsBase != NO_INDEX) {
        leds.setPixelColor(ledIndex(digitHundredsBase, hundredsDigit), (foundNonZeroDigit) ? color : colorBlack);
    }

    foundNonZeroDigit |= tensDigit != 0;
    if (digitTensBase != NO_INDEX) {
        leds.setPixelColor(ledIndex(digitTensBase, tensDigit), (foundNonZeroDigit) ? color : colorBlack);
    }

    // We never show nothing, so if we've gotten all the way down here
    // and the last digit is 0 we still show it.
    foundNonZeroDigit |= onesDigit != 0;
    if (digitOnesBase != NO_INDEX) {
        leds.setPixelColor(ledIndex(digitOnesBase, onesDigit), color);
    }

    leds.show();
}

#define USER_TIMEOUT 1
#define USER_HIT_SET 2

// Simple color scale so we can do color brightness animations.
uint32_t scaleColor(uint32_t color, uint8_t scale)
{
    // The cast chops off the high bits without having to mask them.
    uint8_t r = (uint8_t)(color >> 16);
    uint8_t g = (uint8_t)(color >> 8);
    uint8_t b = (uint8_t)(color);
   
    r = (r*scale) >> 8;
    g = (g*scale) >> 8;
    b = (b*scale) >> 8;

    return ((uint32_t )r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
    

uint8_t setValueUsingButtons(int *currentValue, int minValue, int maxValue,
                          uint8_t digitOnesBase,
                          uint8_t digitTensBase,
                          uint8_t digitHundredsBase,
                          uint8_t digitThousandsBase,
                          uint8_t digitType)
{
    SharedEventState eventState;
    ButtonInfo setButtonInfo;
    ButtonInfo upButtonInfo;
    ButtonInfo downButtonInfo;
    
    clearSharedEventState(&eventState);
    initializeButtonInfo(&setButtonInfo, SWITCH_SET_DIG_IN, false);
    initializeButtonInfo(&upButtonInfo, SWITCH_UP_DIG_IN, true);
    initializeButtonInfo(&downButtonInfo, SWITCH_DOWN_DIG_IN, true);

    displayValue(*currentValue, digitOnesBase, digitTensBase, digitHundredsBase, digitThousandsBase, 255, digitType);

    time_t startTime = millis();
    time_t currentTime = startTime;
    time_t mostRecentEventTime = currentTime;
  
    uint8_t event;
    uint8_t colorScale;
  
    do {
        currentTime = millis();

        // Compute a saw tooth that cycles every second from 255 down to 205 and then back up to 255
        // so we can "blink" the digits you are setting in a way that's familar to clock setting people.
        colorScale = 255 - (((currentTime - startTime)%500)/2);
        //   DEBUG_PRINT(F("scale: "));
        //   DEBUG_PRINTLN(colorScale);
        displayValue(*currentValue, digitOnesBase, digitTensBase, digitHundredsBase, digitThousandsBase, colorScale, digitType);

        event = updateButtonInfoWithCurrentState(currentTime, &eventState, &setButtonInfo);     
        if (event != BUTTON_NO_EVENT) {
            if (event == BUTTON_PRESSED) {
                DEBUG_PRINTLN(F("user hit SET"));
                return USER_HIT_SET;
            }
        } else {
            event = updateButtonInfoWithCurrentState(currentTime, &eventState, &upButtonInfo);
            if (event != BUTTON_NO_EVENT) {
                DEBUG_PRINT(F("user UP event: "));
                DEBUG_PRINTLN(event);
                if (event == BUTTON_PRESSED || event == BUTTON_AUTO_REPEAT) {   
                    if (*currentValue >= maxValue) {
                        *currentValue = minValue;
                    } else {
                        (*currentValue)++;
                    }
                    DEBUG_PRINT(F("VALUE: "));
                    DEBUG_PRINTLN((*currentValue));
                    mostRecentEventTime = currentTime;
                    displayValue(*currentValue, digitOnesBase, digitTensBase, digitHundredsBase, digitThousandsBase, colorScale, digitType);
                }
            } else {
                event = updateButtonInfoWithCurrentState(currentTime, &eventState, &downButtonInfo);
                if (event != BUTTON_NO_EVENT) {
                    DEBUG_PRINT(F("user DOWN event: "));
                    DEBUG_PRINTLN(event);
                    if (event == BUTTON_PRESSED || event == BUTTON_AUTO_REPEAT) {
                        // check before decrement so minValue 0 doesn't wrap.
                        if (*currentValue <= minValue) {
                            // Keep them from wrapping around on the year since going up to
                            // max year is useless and disorienting.
                            if (minValue != 2016) {
                                *currentValue = maxValue;
                            } else {
                                // Otherwise just clamp
                                *currentValue = minValue;
                            }
                        } else {
                            (*currentValue)--;
                        }
                        DEBUG_PRINT(F("VALUE: "));
                        DEBUG_PRINTLN((*currentValue));
                        mostRecentEventTime = currentTime;
                        displayValue(*currentValue, digitOnesBase, digitTensBase, digitHundredsBase, digitThousandsBase, colorScale, digitType);
                    }
                }
            }
        }
    } while (currentTime - mostRecentEventTime < USER_INPUT_TIMEOUT);
    
    return USER_TIMEOUT;
}


void applyNewValues(time_t timeAtStart, int newHours, int newMinutes, int newGMTOffset, int newYear, int newMonth, int newDay)
{
    tmElements_t tm;
    
    if (newMinutes == minute(timeAtStart) &&
        newGMTOffset == gInfo.gmtOffsetInSeconds &&
        newYear == year(timeAtStart) &&
        newMonth == month(timeAtStart) &&
        newDay == day(timeAtStart)) {
       
        // User did nothing.
        if (newHours == hour(timeAtStart)) {
            return;
        }
        
        // User only changed the hour, assume this is a daylight savings time adjustment
        // and fiddle with the gMT offset intead of fully setting the time.
        int changeInHours = newHours - hour(timeAtStart);
        
        if (abs(changeInHours) == 1) {
            gInfo.gmtOffsetInSeconds += changeInHours*60*60;
            writeEEProm(timeAtStart);
            return;
        }
    }
    
    tm.Year = CalendarYrToTm(newYear);
    tm.Month = newMonth;
    tm.Day = newDay;
    tm.Hour = newHours;
    tm.Minute = newMinutes;
    tm.Second = 0;

    gInfo.gmtOffsetInSeconds = newGMTOffset;
    
    time_t t = makeTime(tm) - gInfo.gmtOffsetInSeconds;
    RTC.set(t);        //use the time_t value to ensure correct weekday is set
    setTime(t);

    // Save the GMT offset/time off into eeprom.
    writeEEProm(t);
}

void redrawTime(time_t t)
{
    clearLeds();
    setLedsWithTime(hour(t), minute(t));
    setMoonPhaseLeds(year(t), month(t), day(t), hour(t));
    leds.show();
}
   
void performUserSetupSequence()
{
    time_t timeAtStart = now() + gInfo.gmtOffsetInSeconds;
    
    int newHours = hour(timeAtStart);
    int newMinutes = minute(timeAtStart);
    int newGMTOffset = gInfo.gmtOffsetInSeconds/(60*60);
    int newYear = year(timeAtStart);
    int newMonth = month(timeAtStart);
    int newDay = day(timeAtStart);

    DEBUG_PRINTLN(F("setup sequence"));
    uint8_t result = setValueUsingButtons(&newHours, 0, 23, HOUR_ONES_BASE, HOUR_TENS_BASE, NO_INDEX, NO_INDEX, DIGIT_TYPE_HOURS);
    redrawTime(timeAtStart);
    
    if (result != USER_TIMEOUT) {
        result = setValueUsingButtons(&newMinutes, 0, 59, MINUTE_ONES_BASE, MINUTE_TENS_BASE, NO_INDEX, NO_INDEX, DIGIT_TYPE_MINUTES);
    }

    // GMT offset zones seem to go from -11 to + 14
    if (result != USER_TIMEOUT) {
        result = setValueUsingButtons(&newGMTOffset, -11.0, 14.0, MINUTE_ONES_BASE, MINUTE_TENS_BASE, HOUR_ONES_BASE, HOUR_TENS_BASE, DIGIT_TYPE_GMT_OFFSET);
    }
    
    if (result != USER_TIMEOUT) {
        result = setValueUsingButtons(&newYear, 2016, 2116, MINUTE_ONES_BASE, MINUTE_TENS_BASE, HOUR_ONES_BASE, HOUR_TENS_BASE, DIGIT_TYPE_YEAR);
    }
    
    if (result != USER_TIMEOUT) {
        result = setValueUsingButtons(&newMonth, 1, 12, MINUTE_ONES_BASE, MINUTE_TENS_BASE, HOUR_ONES_BASE, HOUR_TENS_BASE, DIGIT_TYPE_MONTH);
    }
    
    // To save code space we don't enfoce month lengths so this value could be wrong for the chosen month.
    if (result != USER_TIMEOUT) {
        result = setValueUsingButtons(&newDay, 1, 31, MINUTE_ONES_BASE, MINUTE_TENS_BASE, NO_INDEX, NO_INDEX, DIGIT_TYPE_DAY);
    }

    applyNewValues(timeAtStart, newHours, newMinutes,  newGMTOffset*60*60, newYear, newMonth, newDay);

    // We update what's displayed but wait a second so users won't re-trigger set accedently.
    showCurrentMoonAndTime(now() + gInfo.gmtOffsetInSeconds);
    delay(1000);
}

void flagWaveAnimation()
{
    // Flag pole gray, then blue white and red flag.
    gHourTensColor = 0x555510; // Dimmer white.
    gHourOnesColor = 0x0000ff; // Blue
    gMinTensColor = 0xffffaa;  // Warm White
    gMinOnesColor = 0xff0000;  // Red

    float value = ((float)(millis()%2000)/2000.0)*M_PI*2.0;

    // Have them "flap" more the further they are from the flag pole.
    gHourOnesScale = 255 - (int)(((sin(value) + 1.0)/2.0)*110.0);
    gMinTensScale = 255 -  (int)(((sin(value + M_PI/2.0) + 1.0)/2.0)*150.0);
    gMinOnesScale = 255 - (int)(((sin(value + M_PI) + 1.0)/2.0)*200.0);
}

void eyeAnimation()
{
    clearSpecialCharacterLeds();
    leds.setPixelColor(EYE_OUTLINE,  leds.Color(0, 200, 0));
    
    static int state = 1;
    static int seakDuration = EYE_SEAK_DURATION;
    static int goal = 1;
    static int duration = 10;
    static int value = 229;

    uint32_t goldColor = leds.Color(128, 50, 0);
    uint32_t yellowColor = leds.Color(255, 200, 0);

    if (millis()%10 == 0) {
        value = 0;
        for (int i=0; i<12; ++i) {
            value += random(120, 255);
        }
        value = value/12;

    }

    yellowColor = scaleColor(yellowColor, value);
    goldColor = scaleColor(goldColor, value);
    
    if (state == 0) {
        leds.setPixelColor(ZERO_PERCENT_MOON_ARC, yellowColor);
        leds.setPixelColor(TWENTY_PERCENT_MOON_ARC, goldColor);
        leds.setPixelColor(FORTY_PERCENT_MOON_ARC, goldColor);
        leds.setPixelColor(SIXTY_PERCENT_MOON_ARC, yellowColor);
    } else if (state == 1) {
        leds.setPixelColor(TWENTY_PERCENT_MOON_ARC, yellowColor);
        leds.setPixelColor(FORTY_PERCENT_MOON_ARC, goldColor);
        leds.setPixelColor(SIXTY_PERCENT_MOON_ARC, goldColor);
        leds.setPixelColor(EIGHTY_PERCENT_MOON_ARC, yellowColor);
    } else if (state == 2) {
        leds.setPixelColor(FORTY_PERCENT_MOON_ARC, yellowColor);
        leds.setPixelColor(SIXTY_PERCENT_MOON_ARC, goldColor);
        leds.setPixelColor(EIGHTY_PERCENT_MOON_ARC, goldColor);
        leds.setPixelColor(ONE_HUNDRED_PERCENT_MOON_ARC, yellowColor);
    }
    
    if (state > goal) {
        if (seakDuration > 0) {
            seakDuration--;
        } else {
            state--;
            seakDuration = EYE_SEAK_DURATION;            
        }
    } else if (state < goal) {
        if (seakDuration > 0) {
            seakDuration--;
        } else {
            state++;
            seakDuration = EYE_SEAK_DURATION;
        }
    } else {
        if (duration > 0) {
            duration--;
        } else {
            goal = (rand()%2) ? 1 : (rand()%2) ? 2 : 0;
            duration = (goal == 1) ? random(800, 6000) : random(800, 1500);
            seakDuration = EYE_SEAK_DURATION;
        }
    }
}

void doColorSpin(uint32_t colorArray[], uint8_t count, bool left)
{
    unsigned char indexArray[] = {7, 5, 3, 4, 6, 8};
    static unsigned char offset = 0;
    
    for (int i=0; i<6; ++i) {
        leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[ (int)indexArray[i]], colorArray[(i + offset)%count]);
    }
    
    if (left) {
        offset = (offset + 1)%count;
    } else {
        if (offset == 0) {
            offset = count - 1;
        } else {
            offset--;
        }
    }
}

void spinGlobe()
{
    uint32_t globColors[] = { 0x8888ff,
                            0xffffff,
                            0x8888ff,
                            0x3333ff,
                            0x8888ff,
                            0xaaccff,
                            0x8888ff,
                            0x3333ff,
                            0x0000ff,
                            0x0000ff,
                            0x0066ff,
                            0x0000ff };
    
    doColorSpin(globColors, sizeof(globColors)/sizeof(uint32_t), false);
    delay(175);
}


void goldenRingSpin()
{
    static uint8_t step = 0;

    //    clearSpecialCharacterLeds();

    uint32_t frontColor = leds.Color(255, 200, 0);
    uint32_t backColor = frontColor;
    
    switch(step) {
        case 0:
            leds.setPixelColor(ZERO_PERCENT_MOON_ARC, scaleColor(frontColor, 128));
            leds.setPixelColor(ONE_HUNDRED_PERCENT_MOON_ARC, scaleColor(backColor, 128));
            break;
        case 1:
            leds.setPixelColor(TWENTY_PERCENT_MOON_ARC, scaleColor(frontColor, 204));
            leds.setPixelColor(EIGHTY_PERCENT_MOON_ARC, scaleColor(backColor, 102));
            break;
        case 2:
            leds.setPixelColor(FORTY_PERCENT_MOON_ARC, frontColor);
            leds.setPixelColor(SIXTY_PERCENT_MOON_ARC, scaleColor(backColor, 51));
            break;
        case 3:
            leds.setPixelColor(FORTY_PERCENT_MOON_ARC, scaleColor(backColor, 51));
            leds.setPixelColor(SIXTY_PERCENT_MOON_ARC, frontColor);
            break;
        case 4:
            leds.setPixelColor(TWENTY_PERCENT_MOON_ARC, scaleColor(backColor, 102));
            leds.setPixelColor(EIGHTY_PERCENT_MOON_ARC, scaleColor(frontColor, 204));
            break;
        case 5:
            leds.setPixelColor(ZERO_PERCENT_MOON_ARC, scaleColor(backColor, 128));
            leds.setPixelColor(ONE_HUNDRED_PERCENT_MOON_ARC, scaleColor(frontColor, 128));
            break;
    }
    
    step = (step + 1)%5;
    delay(150);
}
