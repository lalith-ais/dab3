#include "TFT_eSPI.h"
#include <RotaryEncoder.h>
#include "OpenFontRender.h"
#include "binaryttf.h"
#include "driver/uart.h"

#define SCREEN_WIDTH 320

// rotary encoder ports
#define RE_CLK 36 // CLK 
#define RE_SW  35 // PUSH SWITCH (GPIO22 is pulled up + green LED)
#define RE_DATA 39 // DATA OUT

//T4B register definitions
#define GPIO_43	0x2B
#define GPIO_55 0x37
#define GPIO_54 0x36
#define GPIO_53	0x35
#define GPIO_22 0x16
#define i2s_DATA 0x07
#define i2s_LRCLK 0x08
#define i2s_MCLK 0x09
#define i2s_BCLK 0x0A
#define SPDIF 0x0B

// note TX, RX pins are swapped - pcb error
#define BUF_SIZE 1024
#define UART_TX_PIN 16
#define UART_RX_PIN 17

RotaryEncoder *encoder = nullptr;
static int pos = 0;
char  buf[15];
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

OpenFontRender render;
char utf8Text[256]; 
size_t length = 0;
uint8_t data[256];
char rxdata[256]; // buffer for misc strings
uint32_t cursorX;
uint32_t cursorY=0;
int totalChannels = 0;
int channel = 0x00;
unsigned char status;
unsigned char last_status = 0x03;
bool update_flag = false;
hw_timer_t * timer = NULL;
int  num_int = 0;
int count = 0;

// timer0 ISR 
void IRAM_ATTR onTimer(){
	num_int++ ;
}

// rotary encoder ISR
IRAM_ATTR void checkPosition()
{
	encoder->tick(); // just call tick() to check the state.
}

// T4B helpers

void writeReadUart(const char* _cmd, const size_t& _size, uint8_t _timeout)
{
	uart_flush(UART_NUM_2);
	uart_write_bytes(UART_NUM_2, _cmd , _size);
	delay(_timeout);
	uart_get_buffered_data_len(UART_NUM_2, &length);
	if (length > 0) {
		length = uart_read_bytes(UART_NUM_2, data, length, 100);
		uart_flush(UART_NUM_2);
	}
}

void SYSTEM_Reset(unsigned char mode) {
	// mode 0: reboot, 1:clear database & reboot, 2: clear database
	const char command[8] = {0xFE, 0x00, 0x01, 0x01, 0x00, 0x01, mode, 0xFD};
	writeReadUart(command, 8, 100);
}


bool SYSTEM_GetSysRdy(void)
{
	bool status = false;
	const char command[7] = {0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFD};
	writeReadUart(command, 7, 50);
	if ( data [2] == 0x01 ) {
		status = true ;
	}
	return (status) ;
}

void SYSTEM_GetAllVersion(void) {
	int index = 0 ;
	const char command[7] = {0xFE, 0x00, 0x05, 0x01, 0x00, 0x00,  0xFD};
	writeReadUart(command, 7, 50); 
	for (int z = 6; z < 15; z++) {
		sprintf(&rxdata[index], "%02X ", data[z]);
		index += 3; // Advance index by 2 chars for hex + 1 space
	}
	rxdata[index - 1] = '\0'; // rxdata can now be printed
}

int STREAM_GetTotalProgram (void) {
	const char command[] =  {0xFE, 0x01, 0x13, 0x13, 0x00, 0x00, 0xFD};
	writeReadUart(command, 7, 100);
	return data[9]; // max 200 channels, hence one byte is ok
}

// main

void setup () {
	Serial.begin(115200);
	Serial.println("starting up..");

	uart_config_t uart_config = {
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE
	};
	uart_param_config(UART_NUM_2, &uart_config);
	uart_set_pin(UART_NUM_2, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	uart_driver_install(UART_NUM_2, BUF_SIZE, BUF_SIZE, 20, NULL, 0);
	uart_flush(UART_NUM_2);

	tft.begin();
	tft.setRotation(3);
	tft.fillScreen(TFT_BLACK);
	tft.setSwapBytes(false);
	tft.invertDisplay(1);
	render.loadFont(binaryttf, sizeof(binaryttf));
	spr.createSprite(320, 20);  // status bar, 14 font (0,170) 
	render.setDrawer(spr);
	render.setCursor(0, 0);
	render.setFontSize(14);
	render.setFontColor(TFT_ORANGE);
	render.printf("initialising T4B    ");
	spr.pushSprite(0,170);
	spr.deleteSprite();
	Serial.println("initialising T4B");
	SYSTEM_Reset(0x00);
	delay(3000);
	while (! SYSTEM_GetSysRdy()) {
		delay(500);
		Serial.print(".");
	}
	SYSTEM_GetAllVersion();
	spr.createSprite(320, 20);  // status bar, 14 font
	render.setDrawer(spr);
	render.setCursor(0, 0);
	render.setFontSize(14);
	render.setCursor(0, 0);
	render.setFontColor(TFT_GREEN);
	render.printf("fw version:  ");
	render.setFontColor(TFT_BLUE);
	render.printf(rxdata);
	spr.pushSprite(0,170);
	spr.deleteSprite();

	totalChannels = STREAM_GetTotalProgram();
	Serial.print("total channels :");
	Serial.println(totalChannels);


	encoder = new RotaryEncoder(RE_DATA, RE_CLK, RotaryEncoder::LatchMode::TWO03);

	attachInterrupt(digitalPinToInterrupt(RE_DATA), checkPosition, CHANGE);
	attachInterrupt(digitalPinToInterrupt(RE_CLK), checkPosition, CHANGE);

} // setup

void loop() {

	int newPos = encoder->getPosition();
	if (pos != newPos) {
		if (newPos > pos) { channel++ ; } else { channel-- ; }
		if (channel < 0 ) { channel = 0 ;}
		if ( channel ==  totalChannels ) { channel-- ;}
		pos = newPos;
		Serial.println(channel);
	}



} // loop
