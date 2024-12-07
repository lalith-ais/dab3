#include "TFT_eSPI.h"
#include <RotaryEncoder.h>
#include "OpenFontRender.h"
#include "binaryttf.h"
#include "driver/uart.h"

#define SCREEN_WIDTH 320

// Node to store each line in a linked list
typedef struct LineNode {
	char* line;
	struct LineNode* next;
} LineNode;

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
unsigned char channel = 0x00;
unsigned char status;
unsigned char last_status = 0x03;
bool update_flag = false;
hw_timer_t * timer = NULL;
int  num_int = 0;
int count = 0;
uint8_t status_flag = 0;


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

// Function to split text by width with font size set once
LineNode* splitByWidth(const char* input, int fontSize) {
	render.setFontSize(fontSize); // Set the font size once

	LineNode* head = NULL; // Start of the linked list
	LineNode* tail = NULL; // End of the linked list

	// Make a copy of the input to avoid modifying the original string
	size_t length = strlen(input);
	char* buffer = (char*)malloc(length + 1);
	if (!buffer) return NULL; // Memory allocation check
	strcpy(buffer, input);

	char* token = strtok(buffer, " "); // Tokenize the input by spaces
	char* currentLine = (char*)malloc(length + 1); // Temporary buffer for the current line
	if (!currentLine) {
		free(buffer);
		return NULL;
	}
	currentLine[0] = '\0'; // Start with an empty string

	while (token != NULL) {
		// Test if adding the current token fits within SCREEN_WIDTH
		size_t newLineLength = strlen(currentLine) + strlen(token) + 1; // +1 for space
		char* newLine = (char*)malloc(newLineLength + 1); // Temporary line with token
		if (!newLine) break;
		strcpy(newLine, currentLine);
		if (strlen(currentLine) > 0) strcat(newLine, " ");
		strcat(newLine, token);

		if (render.getTextWidth(newLine) <= SCREEN_WIDTH) {
			// If it fits, update the current line
			strcpy(currentLine, newLine);
		} else {
			// If it doesn't fit, add the current line to the list
			LineNode* newNode = (LineNode*)malloc(sizeof(LineNode));
			if (!newNode) break;
			newNode->line = strdup(currentLine);
			newNode->next = NULL;

			if (!head) head = tail = newNode; // First node
			else {
				tail->next = newNode;
				tail = newNode;
			}

			// Start a new line with the current token
			strcpy(currentLine, token);
		}
		free(newLine); // Clean up temporary line
		token = strtok(NULL, " ");
	}

	// Add the last line if there's any
	if (strlen(currentLine) > 0) {
		LineNode* newNode = (LineNode*)malloc(sizeof(LineNode));
		if (newNode) {
			newNode->line = strdup(currentLine);
			newNode->next = NULL;

			if (!head) head = newNode;
			else tail->next = newNode;
		}
	}

	// Cleanup
	free(currentLine);
	free(buffer);
	return head;
}

// Helper function to print the linked list
void printLines(LineNode* head) {
	while (head) {
		int textWidth = render.getTextWidth(head->line);
		int textHeight = render.getTextHeight(head->line);
		int cursorX = (SCREEN_WIDTH - textWidth) / 2;
		render.setCursor(cursorX, cursorY);
		render.printf(head->line);
		cursorY += (textHeight + 5);
		head = head->next;
	}
}

// Function to render text with variable font size
void renderTextWithFontSize(const char* text, int fontSize) {
	LineNode* lines = splitByWidth(text, fontSize);
	if (lines) {
		printLines(lines);
		freeLines(lines);
	}
}

// Helper function to free the linked list
void freeLines(LineNode* head) {
	while (head) {
		LineNode* temp = head;
		head = head->next;
		free(temp->line);
		free(temp);
	}
}

// T4B text is in UCS2
void convertUCS2toUTF8(uint8_t* ucs2Data, int length, char* utf8Buffer, int utf8BufferSize) {
	int utf8Index = 0;
	// actaul data starts from 6  and ends at length-1
	for (int i = 6; i < length -1; i += 2) {
		// Combine two bytes to form a UCS-2 character
		uint16_t ucs2Char = (ucs2Data[i] << 8) | ucs2Data[i + 1];

		// Convert UCS-2 character to UTF-8 encoding
		if (ucs2Char < 0x80) {
			// 1-byte UTF-8 (ASCII-compatible)
			if (utf8Index < utf8BufferSize - 1) {
				utf8Buffer[utf8Index++] = ucs2Char;
			}
		} else if (ucs2Char < 0x800) {
			// 2-byte UTF-8
			if (utf8Index < utf8BufferSize - 2) {
				utf8Buffer[utf8Index++] = 0xC0 | (ucs2Char >> 6);           // First byte
				utf8Buffer[utf8Index++] = 0x80 | (ucs2Char & 0x3F);         // Second byte
			}
		} else {
			// 3-byte UTF-8
			if (utf8Index < utf8BufferSize - 3) {
				utf8Buffer[utf8Index++] = 0xE0 | (ucs2Char >> 12);          // First byte
				utf8Buffer[utf8Index++] = 0x80 | ((ucs2Char >> 6) & 0x3F);  // Second byte
				utf8Buffer[utf8Index++] = 0x80 | (ucs2Char & 0x3F);         // Third byte
			}
		}
	}

	// Null-terminate the UTF-8 string
	utf8Buffer[utf8Index] = '\0';
}

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

void STREAM_GetProgrameName (unsigned char channel) {
	const char command[12] = {0xFE, 0x01, 0x2d, 0x2d, 0x00, 0x05, 0x00, 0x00, 0x00, channel, 0x01, 0xFD};
	writeReadUart(command, 12, 50);
	convertUCS2toUTF8(data, length, utf8Text, sizeof(utf8Text));
	// utf8Text can be directly written to TFT
	//Serial.println(utf8Text);
	spr.createSprite(320, 40);
	render.setDrawer(spr);
	render.setFontColor(TFT_BLUE); // note font colour must be set as render.setfont
	cursorY=0; // this is required to start writing from top of sprite
	int fontSize =32 ;
	renderTextWithFontSize(utf8Text, fontSize);
	spr.pushSprite(0,35);
	spr.deleteSprite();
}

void STREAM_GetEnsembleName (unsigned char channel) {

	const char command[12] = {0xFE, 0x01, 0x41, 0x41, 0x00, 0x05, 0x00, 0x00, 0x00, channel, 0x01, 0xFD};
	writeReadUart(command, 12, 50);
	convertUCS2toUTF8(data, length, utf8Text, sizeof(utf8Text));
	//Serial.println(utf8Text);
	spr.createSprite(320, 40);
	render.setDrawer(spr);
	render.setFontColor(TFT_CYAN); // note font colour must be set as render.se
	cursorY=0; // this is required to start writing from top of sprite
	int fontSize =18 ;
	renderTextWithFontSize(utf8Text, fontSize);
	spr.pushSprite(0,75);
	spr.deleteSprite();
}

unsigned char STREAM_GetPlayStatus(void) {
	const char command[7] =  {0xFE, 0x01, 0x10, 0x10, 0x00, 0x00, 0xFD};
	writeReadUart(command, 7, 100);
	status_flag= 0;
	status_flag= data[8] ;
	return data[6]; // 0=playing,1=searching,2=tuning,3=stream stop
}


void STREAM_GetProgrameText(unsigned char channel) {

	const char command[7] = {0xFE, 0x01, 0x2e, 0x2e, 0x00, 0x00, 0xFD};
	writeReadUart(command, 7, 50);
	convertUCS2toUTF8(data, length, utf8Text, sizeof(utf8Text));
	// utf8Text can be directly written to TFT
	Serial.println(utf8Text);
	spr.createSprite(320, 80);
	render.setDrawer(spr);
	render.setFontColor(TFT_GREEN); // note font colour must be set as render.setfont
	int fontSize =18 ;
	renderTextWithFontSize(utf8Text, fontSize);
	spr.pushSprite(0,90);
	spr.deleteSprite();

}


// state machine
void CheckStatus (void) {
	status = STREAM_GetPlayStatus();
	switch (status) {
		case 0x00:
			if (status_flag & 0x02) { STREAM_GetProgrameText(channel);}
			if (status != last_status ) {
				STREAM_GetProgrameText(channel);
				last_status = status ;
			}
			break;

		case 0x01:

			break;

		case 0x02:

			break;

		case 0x03:

			break;
	}


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
	Serial.print("playing channel :");
	STREAM_GetProgrameName(channel);
	STREAM_GetEnsembleName(channel);



	encoder = new RotaryEncoder(RE_DATA, RE_CLK, RotaryEncoder::LatchMode::TWO03);
	attachInterrupt(digitalPinToInterrupt(RE_DATA), checkPosition, CHANGE);
	attachInterrupt(digitalPinToInterrupt(RE_CLK), checkPosition, CHANGE);

	// new timer API 3.0 , interrupt every second
	timer = timerBegin(1000000);
	timerAttachInterrupt(timer,  &onTimer );
	timerAlarm(timer, 1000000 , true, 0);

} // setup

void loop() {
	int newPos = encoder->getPosition();
	if (pos != newPos) {
		if (newPos > pos) { channel++ ; } else { channel-- ; }
		if (channel == 0xFF ) { channel = 0x00 ;}
		if ( channel ==  totalChannels ) { channel-- ;}
		pos = newPos;
		STREAM_GetProgrameName(channel);
		STREAM_GetEnsembleName(channel);
		spr.createSprite(320, 20);
		render.setDrawer(spr);
		render.setFontSize(14);
		render.setCursor(0, 0);
		render.setFontColor(TFT_WHITE);
		sprintf(&rxdata[0], "%3u / %3u", channel+1, totalChannels);
		rxdata[10] = '\0'; 
		render.printf(rxdata);
		spr.pushSprite(0,170); 
		spr.deleteSprite(); 
	}

	//tasker  every second
	if ( num_int > 0){
		num_int--;
		//count++ ;
		CheckStatus();
	}

} // loop
