
#define ENABLE_DIAGPRINT                    // expand DPRINT into debug output
//#define ENABLE_DIAGPRINT_VERBOSE            // expand DPRINT and DPRINT_V into debug output
#define ENABLE_ASSERT
#include <lqdiag.h>

/* specify the pin configuration 
 * --------------------------------------------------------------------------------------------- */
#ifdef ARDUINO_ARCH_ESP32
    #define HOST_ESP32_DEVMOD_BMS2
#else
    #define HOST_FEATHER_UXPLOR_L
    // #define HOST_FEATHER_UXPLOR             
    // #define HOST_FEATHER_LTEM3F
#endif

#include <ltemc.h>
#include <ltemc-internal.h>                                             // need internal references, low-level test here
#include <ltemc-nxp-sc16is.h>

// test controls
uint16_t loopCnt = 0;
uint16_t cycle_interval = 3000;
uint32_t lastCycle;
uint8_t testPattern;

//#define HALT_ON_FAULT
uint16_t byteFaults = 0;
uint16_t wordFaults = 0;
uint16_t bytesFaults = 0;
uint16_t registerFaults = 0;


// test needs access to modem instance
extern ltemDevice_t g_lqLTEM;

union regBuffer { uint16_t val; struct { uint8_t msb; uint8_t lsb; }; };
regBuffer txWord;
regBuffer rxWord;
uint8_t txBffr[2];
uint8_t rxBffr[2];


void setup() 
{
    #ifdef DIAGPRINT_SERIAL
        Serial.begin(115200);
        delay(5000);                // just give it some time
    #endif
    DPRINT(PRNT_RED, "LTEmC - Test #2: UART and BGx Communications\r\n");
    randomSeed(analogRead(7));

    configIO();

    DPRINT(PRNT_DEFAULT, "Modem status(%i) = %i \r\n", ltem_pinConfig.statusPin, platform_readPin(ltem_pinConfig.statusPin));
    if (!platform_readPin(ltem_pinConfig.statusPin))
    {
        powerModemOn();
        pDelay(500);
    }
    powerModemOff();
    pDelay(500);
    DPRINT(PRNT_INFO, "Turning modem back on for IO tests\r\n");
    powerModemOn();

    spi_start(g_lqLTEM.platformSpi);
    pDelay(10);

    txWord.msb = SC16IS7xx_UARTRST_regAddr << 3;                                // reset UART to a known state, direct SPI I/O
    txWord.lsb = SC16IS7xx__SW_resetMask;
    spi_transferWord(g_lqLTEM.platformSpi, txWord.val);

    uartTests();

    if (!byteFaults && !wordFaults && !bytesFaults && !registerFaults )         // if basic SPI functional to/from SPI-UART
    {
        SC16IS7xx_start();                                                      // initialize SPI-UART for communications with BGx
    }
    lastCycle = cycle_interval;
}


void loop() 
{
    if (IS_ELAPSED(lastCycle, cycle_interval))
    {
        loopCnt++;
        // DPRINT(PRNT_CYAN,"\n\nLoop=%d FAULTS: byte=%d, word=%d, bytes=%d\r\n", loopCnt, byteFaults, wordFaults, bytesFaults);
        lastCycle = millis();

        /* BG96 test pattern: get IMEI
        *  AT+GSN\r\r
        *  <IMEI value (20 char)>\r\r
        *  OK\r
        */

        // BGx Test

        uint8_t regValue = 0;
        // char cmd[] = "GSN\r\0";                          // less than one SPI-UART buffer (test for slow servicing)
        char cmd[] = "ATI\r\0";                             // returns a 
        // char cmd[] = "AT+QPOWD=0\r\0";                   // if others fail, but this powers down modem, RX failing
        DPRINT(PRNT_DEFAULT, "Invoking cmd: %s \r\n", cmd);

        sendCommand(cmd);                                   // send command and wait ~400ms for BGx response in FIFO buffer

        
        char response[240] = {0};                           // new buffer every loop
        recvResponse(response);

        //\r\nQuectel\r\nBG96\r\nRevision: BG96MAR02A07M1G\r\n\r\nOK\r\n", 
        //ATI\r\r\nQuectel\r\nBG77\r\nRevision: BG77LAR02A04\r\n\r\nOK\r\n"
        // test response v. expected 

        const char* validResponse = "\r\nQuectel\r\nBG";                                              // initial characters in response

        if (strlen(response) == 0)
        {
            DPRINT(PRNT_WARN, "Got no response from BGx.\r\n");
        }
        if (strstr(response, "APP RDY"))
        {
            DPRINT(PRNT_WARN, "Received APP RDY from LTEm.\r\n");
        }
        else if (strlen(response) > 40 )
        {
            if (strstr(response, validResponse) && strstr(response, "OK\r\n"))
            {
                DPRINT(PRNT_DEFAULT, "Got correctly formed response: \r\n%s", response);  
            }
            else
                indicateFailure("Unexpected device information returned on cmd test... failed."); 
        }
        DPRINT(PRNT_DEFAULT,"Loop=%d \n\n", loopCnt);
    }
}


#pragma region helpers
/* =========================================================================================================================
========================================================================================================================= */

void powerModemOn()
{
	if (!platform_readPin(ltem_pinConfig.statusPin))
	{
		DPRINT(0, "Powering LTEm On...");
		platform_writePin(ltem_pinConfig.powerkeyPin, gpioValue_high);
		pDelay(1000);
		platform_writePin(ltem_pinConfig.powerkeyPin, gpioValue_low);
		while (!platform_readPin(ltem_pinConfig.statusPin))
		{
			pDelay(500);
		}
		DPRINT(0, "DONE.\r\n");
	}
	else
	{
		DPRINT(0, "LTEm is already powered on.\r\n");
	}
}


void powerModemOff()
{
	if (platform_readPin(ltem_pinConfig.statusPin))
	{
		DPRINT(0, "Powering LTEm Off...");
		platform_writePin(ltem_pinConfig.powerkeyPin, gpioValue_high);
		pDelay(1000);
		platform_writePin(ltem_pinConfig.powerkeyPin, gpioValue_low);
		while (platform_readPin(ltem_pinConfig.statusPin))
		{
			pDelay(500);
		}
		DPRINT(0, "DONE.\r\n");
	}
	else
	{
		DPRINT(0, "LTEm is already powered off.\r\n");
	}
}


void configIO()
{
    /* Create a custom pin config if porting to a new device
     */
    // const static ltemPinConfig_t ltem_pinConfig =
    // {
    //     spiIndx : -1,
    //     spiCsPin : 8,    // original: 18
    //     spiClkPin : 16,  // original: 15
    //     spiMisoPin : 17, // original: 16
    //     spiMosiPin : 18, // original: 17
    //     irqPin : 3,      // original: 8
    //     statusPin : 47,
    //     powerkeyPin : 45,
    //     resetPin : 0,
    //     ringUrcPin : 0,
    //     wakePin : 48
    // };

    memcpy(&g_lqLTEM.pinConfig, &ltem_pinConfig, sizeof(ltemPinConfig_t));              // initialize LTEm pinout

	// on Arduino, ensure pin is in default "logical" state prior to opening
	platform_writePin(ltem_pinConfig.powerkeyPin, gpioValue_low);
	platform_writePin(ltem_pinConfig.resetPin, gpioValue_low);
	platform_writePin(ltem_pinConfig.spiCsPin, gpioValue_high);

	platform_openPin(ltem_pinConfig.powerkeyPin, gpioMode_output);		// powerKey: normal low
	platform_openPin(ltem_pinConfig.resetPin, gpioMode_output);			// resetPin: normal low
	platform_openPin(ltem_pinConfig.spiCsPin, gpioMode_output);			// spiCsPin: invert, normal gpioValue_high
	platform_openPin(ltem_pinConfig.statusPin, gpioMode_input);
	platform_openPin(ltem_pinConfig.irqPin, gpioMode_inputPullUp);

    // SPI bus
    #if defined(ARDUINO_ARCH_SAMD)
	g_lqLTEM.platformSpi = spi_createFromIndex(ltem_pinConfig.spiIndx, ltem_pinConfig.spiCsPin);
    #else
    g_lqLTEM.platformSpi = spi_createFromPins(ltem_pinConfig.spiClkPin, ltem_pinConfig.spiMisoPin, ltem_pinConfig.spiMosiPin, ltem_pinConfig.spiCsPin);
    #endif

	if (g_lqLTEM.platformSpi == NULL)
	{
        DPRINT(PRNT_WARN, "SPI create failed.\r\n");
	}
}


void uartTests()
{
    testPattern = random(256);
    DPRINT(0, "  Writing %02X to scratchpad register with transfer BYTE...", testPattern);

    rxWord.msb = (SC16IS7xx_SPR_regAddr << 3) | 0x80;
    // rxBuffer.lsb doesn't matter prior to read
    uint8_t rxData;

    spi_transferBegin(g_lqLTEM.platformSpi);                                     // write scratchpad
    txWord.msb = SC16IS7xx_SPR_regAddr << 3;
    txWord.lsb = testPattern;
    rxData = spi_transferByte(g_lqLTEM.platformSpi, txWord.msb);
    rxData = spi_transferByte(g_lqLTEM.platformSpi, txWord.lsb);
    spi_transferEnd(g_lqLTEM.platformSpi);

    spi_transferBegin(g_lqLTEM.platformSpi);                                     // read scratchpad
    txWord.msb = (SC16IS7xx_SPR_regAddr << 3) | 0x80;                   // register addr + read data bit set
    txWord.lsb = 0;                                                     // doesn't matter on read
    rxData = spi_transferByte(g_lqLTEM.platformSpi, txWord.msb);
    rxData = spi_transferByte(g_lqLTEM.platformSpi, txWord.lsb);
    spi_transferEnd(g_lqLTEM.platformSpi);

    if (testPattern == rxData)
    {
        DPRINT(PRNT_INFO, "Scratchpad transfer BYTE success.\r\n");
    }
    else
    {
        DPRINT(PRNT_WARN, "Scratchpad transfer BYTE write/read failed (expected=%d, got=%d).\r\n", testPattern, rxData);
        byteFaults++;
        #ifdef HALT_ON_FAULT
            while(1){;}
        #endif
    }

    /* ----------------------------------------------------------------------------------------- */
    testPattern = random(256);
    DPRINT(0, "  Writing %02X to scratchpad register with transfer WORD...", testPattern);

    txWord.msb = SC16IS7xx_SPR_regAddr << 3;
    txWord.lsb = testPattern;
    rxWord.msb = (SC16IS7xx_SPR_regAddr << 3) | 0x80;
    // rxBuffer.lsb doesn't matter prior to read

    uint16_t d = spi_transferWord(g_lqLTEM.platformSpi, txWord.val);
    rxWord.val = spi_transferWord(g_lqLTEM.platformSpi, rxWord.val);

    if (testPattern == rxWord.lsb)
    {
        DPRINT(PRNT_INFO, "Scratchpad transfer WORD success.\r\n");
    }
    else
    {
        DPRINT(PRNT_WARN, "Scratchpad transfer WORD write/read failed (expected=%d, got=%d).\r\n", testPattern, rxWord.lsb);
        wordFaults++;
        #ifdef HALT_ON_FAULT
            while(1){;}
        #endif
    }

    /* ----------------------------------------------------------------------------------------- */
    testPattern = random(256);
    DPRINT(0, "  Writing %02X to scratchpad register with transfer BYTES (buffer)...", testPattern);

    // write scratchpad
    txBffr[0] = SC16IS7xx_SPR_regAddr << 3;
    txBffr[1] = testPattern;
    spi_transferBytes(g_lqLTEM.platformSpi, txBffr, NULL, 2);

    // read scratchpad
    txBffr[0] = (SC16IS7xx_SPR_regAddr << 3) | 0x80;                    // write: reg addr + read bit set
    txBffr[1] = 0;                                                      // doesn't matter on read
    spi_transferBytes(g_lqLTEM.platformSpi, txBffr, rxBffr, 2);

    if (rxBffr[1] == testPattern)
    {
        DPRINT(PRNT_INFO, "Scratchpad transfer BYTES (buffer) success.\r\n");
    }
    else
    {
        DPRINT(PRNT_WARN, "Scratchpad transfer BYTES (buffer) write/read failed (expected=%d, got=%d).\r\n", testPattern, rxBffr[1]);
        bytesFaults++;
        #ifdef HALT_ON_FAULT
            while(1){;}
        #endif
    }

    /* ----------------------------------------------------------------------------------------- */
    testPattern = random(256);
    DPRINT(0, "  Writing %02X to scratchpad register with SC16IS7xx I/O...", testPattern);

    SC16IS7xx_writeReg(SC16IS7xx_SPR_regAddr, testPattern);
    uint8_t sprValue = SC16IS7xx_readReg(SC16IS7xx_SPR_regAddr);

    if (testPattern == sprValue)
    {
        DPRINT(PRNT_INFO, "Scratchpad register I/O success.\r\n");
    }
    else
    {
        DPRINT(PRNT_WARN, "Scratchpad register I/O write/read failed (expected=%d, got=%d).\r\n", testPattern, sprValue);
        registerFaults++;
        #ifdef HALT_ON_FAULT
            while(1){;}
        #endif
    }
}


#define ASCII_CR 13U

// This functionality is normally handled in the IOP module. 
// ISR functionality is tested in the ltemc-03-iopisr test
void sendCommand(const char* cmd)
{
    size_t sendSz = strlen(cmd);

    //SC16IS7xx_write(cmd, strlen(cmd));                        // normally you are going to use buffered writes like here

    for (size_t i = 0; i < sendSz; i++)
    {
        SC16IS7xx_writeReg(SC16IS7xx_FIFO_regAddr, cmd[i]);     // without a small delay the register is not moved to FIFO before next byte\char
        pDelay(1);                                              // this is NOT the typical write cycle
    }
    pDelay(300);                                                // max response time per-Quectel specs, for this test we will wait
}


// "Polled" I\O, ensuring SPI-UART and serial lines to BGx are functioning
// This functionality is normally handled in the IOP module's interrupt service routine (ISR). 
// ISR functionality is tested in the ltemc-03-iopisr test
void recvResponse(char *response)
{
    uint8_t recvSz = 0;
    uint16_t recvdLen = 0;

    uint32_t waitStart = pMillis();
    do
    {
        uint8_t lsrValue = SC16IS7xx_readReg(SC16IS7xx_LSR_regAddr);
        if (lsrValue & SC16IS7xx__LSR_RHR_dataReady)
        {
            recvSz = SC16IS7xx_readReg(SC16IS7xx_RXLVL_regAddr);
            SC16IS7xx_read(response + recvdLen, recvSz);
            recvdLen += recvSz;

            if (strstr(response, "OK\r\n"));
                return;
        }

        /* code */
    } while (IS_ELAPSED(waitStart, SEC_TO_MS(1)));
}


void indicateFailure(const char* failureMsg)
{
	DPRINT(PRNT_ERROR, "\r\n** %s \r\n", failureMsg);
    DPRINT(PRNT_ERROR, "** Test Assertion Failed. \r\n");

    int halt = 1;
    DPRINT(PRNT_ERROR, "** Halting Execution \r\n");
    while (halt)
    {
        platform_writePin(LED_BUILTIN, gpioPinValue_t::gpioValue_high);
        delay(1000);
        platform_writePin(LED_BUILTIN, gpioPinValue_t::gpioValue_low);
        delay(100);
    }
}


void _ping(const char *msg)
{
    Serial.println(msg);
    vTaskDelay(10);
}


void _stop(const char *msg)
{
    Serial.println(msg);
    while(1){};
}

#pragma endregion