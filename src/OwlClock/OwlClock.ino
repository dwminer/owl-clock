

#include <Adafruit_NeoPixel.h> // https://github.com/adafruit/Adafruit_NeoPixel
#include <DS3232RTC.h>         // http://github.com/JChristensen/DS3232RTC
#include <Time.h>              // http://www.arduino.cc/playground/Code/Time
#include <Wire.h>              // http://arduino.cc/en/Reference/Wire (included with Arduino IDE)
#include <EEPROM.h>

// My two helper libs that hold the extra precision math, and the astronomical calculations.
extern "C" {
#include "float64.h"
#include "AstronomicalCalculations.h"
}

#define LIGHT_SENSOR_TOP_ANALOG_IN     A0
#define LIGHT_SENSOR_INSIDE_ANALOG_IN  A1
#define LED_COUNT 40

#define LED_DIG_OUT        6

// TODO convert these to 0,1,2 I hope using 0 & 1 isn't a big problem.  :b
#define SWITCH_UP_DIG_IN   4
#define SWITCH_DOWN_DIG_IN 5
#define SWITCH_SET_DIG_IN  7

// Neo pixel interface for all the LEDS is though the single pin LED_DIG_OUT
Adafruit_NeoPixel leds = Adafruit_NeoPixel(LED_COUNT, LED_DIG_OUT, NEO_GRB + NEO_KHZ800);

// digit to LED index for the clock digits. Tens and Ones are sequenced differently.
static const unsigned char gTensDigit[] = {6, 9, 4, 3, 1, 8, 5, 2, 0, 7};
static const unsigned char gOnesDigit[] = {5, 0, 3, 6, 8, 1, 4, 7, 9, 2};

static int gGMTOffsetInSeconds = -8*60*60;

static unsigned short gCurrentYear = 0;
static unsigned char gCurrentMonth = 0;
static unsigned char gCurrentDay = 0;

// To addess each digit use these bases + the above index.
#define HOUR_TENS_BASE    20
#define HOUR_ONES_BASE    30
#define MINUTE_TENS_BASE   0
#define MINUTE_ONES_BASE  10

// The various moon arcs, have digit equivalents, so we list those here.
// The percentages go from upper right to lower left.
#define ZERO_PERCENT_MOON_ARC 8
#define TWENTY_PERCENT_MOON_ARC 6
#define FORTY_PERCENT_MOON_ARC 4
#define SIXTY_PERCENT_MOON_ARC 3
#define EIGHTY_PERCENT_MOON_ARC 5
#define ONE_HUNDRED_PERCENT_MOON_ARC 7

#define EEPROM_INFO_ADDRESS 0
#define EEPROM_NEVER_WAS_SET 255
#define EEPROM_INFO_VERSION 1   // Increment this if you change/reorder the EEPROMInfo


#define DEFAULT_LATITUDE 36.3894     // These are burned into the EEPROM on starup/version bump.
#define DEFAULT_LONGITUDE -122.0819

// I wrote a ton of code to compute these sun events, but the code took 24k of program space,
// so instead I'm squeezing the next 100 years of sun events into these arrays.  The
// values are two bit offsets from their base value.  So 4 days per byte.

const unsigned char springEquinoxArray[] PROGMEM = { 85, 85, 85, 85, 85, 85, 85, 84, 84, 84, 84, 84, 84, 84, 84, 80, 80, 80, 80, 80, 80, 165, 165, 149, 149, };
const unsigned char summerSolsticeArray[] PROGMEM = { 84, 84, 84, 84, 84, 84, 80, 80, 80, 80, 80, 80, 80, 64, 64, 64, 64, 64, 64, 64, 0, 85, 85, 85, 85, };
const unsigned char fallEquinoxArray[] PROGMEM = { 165, 165, 165, 149, 149, 149, 149, 149, 149, 149, 149, 85, 85, 85, 85, 85, 85, 85, 85, 84, 84, 169, 169, 169, 169, };
const unsigned char winterSolsticeArray[] PROGMEM = { 149, 149, 149, 149, 149, 149, 149, 85, 85, 85, 85, 85, 85, 85, 85, 85, 84, 84, 84, 84, 84, 169, 169, 169, 165, };

#define SPRING_EQUINOX_DAY_BASE 19
#define SUMMER_SOLSTICE_DAY_BASE 20
#define FALL_EQUINOX_DAY_BASE 21
#define WINTER_SOLSTICE_DAY_BASE 20


#define SUN_EVENT_YEAR_BASE 2016
#define SUN_EVENT_YEAR_RANGE 100

// This should be passed one of the PROGMEM solar event arrays above with the matching DAY_BASE
unsigned char unpackSunEventDay(unsigned char *pgmArray, unsigned char dayBase, int year)
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

  unsigned char mask = 3 << shift;
  
  return ((pgm_read_byte(&pgmArray[index]) & mask) >> shift) + dayBase;
}

// This is the set of things we keep track of in eeprom.  This is mostly 
// so we can record peruser info, and only have to compute the various
// holidays once a year.
typedef struct {
  unsigned char version;  // incremented if the info struct changes, also used to check
                          // never having been written state.
  
  unsigned short year;    // Most recent handled year, year all the holidays are computed for.
  unsigned char month;    // Most recently handled month.
  unsigned char day;      // Most recently handled day.

  int gmtOffsetInSeconds; 
} EEPROMInfo;

void setup(void)
{
  // TODO: Only switch these to 0, 1, 2 when we're programming the actual boards, 
  // otherwise Serial uses 0 and 1.
  //  pinMode(SWITCH_UP_DIG_IN, INPUT_PULLUP);
  //  pinMode(SWITCH_DOWN_DIG_IN, INPUT_PULLUP);
  //  pinMode(SWITCH_SET_DIG_IN, INPUT_PULLUP);
  
  Serial.begin(19200);

  //setSyncProvider() causes the Time library to synchronize with the
  //external RTC by calling RTC.get() every five minutes by default.
  setSyncProvider(RTC.get);
  
  // if (timeStatus() != timeSet) {
  //   Serial.print(F("RTC Sync setup failed."));
  // }

  readAndValidateEEProm();
 
  leds.begin();
  clearLeds();
  leds.show();
}

void
readAndValidateEEProm()
{
  EEPROMInfo info;
  EEPROM.get(EEPROM_INFO_ADDRESS, info);

  time_t t = now();

  // If we have never saved this info off to eeprom
  // we have to assume the current clock is ok and recompute everything based on that.
  if (info.version != EEPROM_INFO_VERSION) {
    info.version = EEPROM_INFO_VERSION;
    info.year = (unsigned short)year(t);
    info.month = month(t);
    info.day = month(t);

  //  info.latitude = DEFAULT_LATITUDE;
  //  info.longitude = DEFAULT_LONGITUDE;

  //  computeHolidays(&info);
  }

//        bool oscStopped(bool clearOSF = true);  //defaults to clear the OSF bit if argument not supplied


  if (info.year != (unsigned short)year(t)) {
    
  }

  
  
  
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
setLedsWithTime(int hours, int minutes, uint32_t color)
{
  if (minutes > 59) {
    minutes = 59;
  }

  hours = twelveHourValueFrom24HourTime(hours);

  int hourTens = floor(hours/10);
  int hourOnes = hours - 10*hourTens;
  
  if (hourTens > 0) {
    leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[hourTens], color);
  }
  leds.setPixelColor(HOUR_ONES_BASE + gOnesDigit[hourOnes], color);

  int minuteTens = floor(minutes/10);
  int minuteOnes = minutes - minuteTens*10;

  leds.setPixelColor(MINUTE_TENS_BASE + gTensDigit[minuteTens], color);
  leds.setPixelColor(MINUTE_ONES_BASE + gOnesDigit[minuteOnes], color);
}

void 
setLedsWithDigits(int hours, int minutes, uint32_t color)
{
  int hourTens = floor(hours/10);
  int hourOnes = hours - 10*hourTens;
  
  leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[hourTens], color);
  leds.setPixelColor(HOUR_ONES_BASE + gOnesDigit[hourOnes], color);

  int minuteTens = floor(minutes/10);
  int minuteOnes = minutes - minuteTens*10;

  leds.setPixelColor(MINUTE_TENS_BASE + gTensDigit[minuteTens], color);
  leds.setPixelColor(MINUTE_ONES_BASE + gOnesDigit[minuteOnes], color);
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
    setLedsWithTime(minutes, seconds, leds.Color(255, 128, 128));
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
        if (percentIlluminated < 0.05) {
            // New moon, we're done.
        } else {
            // Turn on the upper right rim.
            leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[ZERO_PERCENT_MOON_ARC], moonColor);
            
            if (percentIlluminated < 0.3) {
                leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[TWENTY_PERCENT_MOON_ARC], moonColor);
            } else if (percentIlluminated < 0.5) {
                leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[FORTY_PERCENT_MOON_ARC], moonColor);
            } else if (percentIlluminated < 0.7) {
                leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[SIXTY_PERCENT_MOON_ARC], moonColor);
            } else if (percentIlluminated < 0.95) {
                leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[EIGHTY_PERCENT_MOON_ARC], moonColor);
            } else {
                leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[ONE_HUNDRED_PERCENT_MOON_ARC], moonColor);
            }
        }
    } else {
       // Use the lower left rim.
        if (percentIlluminated < 0.05) {
            // New moon, we're done.
        } else {
            // Turn on the lower left rim.
            leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[ONE_HUNDRED_PERCENT_MOON_ARC], moonColor);
            
            if (percentIlluminated < 0.3) {
                leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[EIGHTY_PERCENT_MOON_ARC], moonColor);
            } else if (percentIlluminated < 0.5) {
                leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[SIXTY_PERCENT_MOON_ARC], moonColor);
            } else if (percentIlluminated < 0.7) {
                leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[FORTY_PERCENT_MOON_ARC], moonColor);
            } else if (percentIlluminated < 0.95) {
                leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[TWENTY_PERCENT_MOON_ARC], moonColor);
            } else {
                leds.setPixelColor(HOUR_TENS_BASE + gTensDigit[ZERO_PERCENT_MOON_ARC], moonColor);
            }
        }
    }
}

void loop()
{
  //   colorTest();
  // while (1) {
  //  test();
  // }

  time_t t = now() + gGMTOffsetInSeconds;
  clearLeds();
  setLedsWithTime(hour(t), minute(t), leds.Color(255, 10, 10));
  setMoonPhaseLeds(year(t), month(t), day(t), hour(t));

  leds.show();
}

void clearLeds()
{
  for (int i=0; i<LED_COUNT; i++) {
    leds.setPixelColor(i, 0);
  }
}

