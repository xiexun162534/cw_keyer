#include <Arduino_FreeRTOS.h>
#include <LiquidCrystal.h>

#define LCD_RS 12
#define LCD_EN 11
#define LCD_D4 5
#define LCD_D5 4
#define LCD_D6 3
#define LCD_D7 2

#define BUZZER 13
#define TONE_FREQ 800

#define PADDLE_LEFT 6
#define PADDLE_RIGHT 7

#define LCD_WIDTH 20
#define LCD_HEIGHT 4

#define LCD_OUTPUT_SIZE LCD_WIDTH

#define MORSE_TABLE_SIZE 0b10000000

static __attribute__((noreturn)) void panic()
{
  while (true) {
    // hang
  }
}

static const unsigned wpm = 13;
static const unsigned dit_ms = 1000 * 60 / (wpm * 50);

static void task_update_lcd(void *args);
static void task_cw(void *args);
static void task_check_paddles(void *args);

enum class paddles_state_t {
  IDLE,
  LEFT,
  RIGHT,
  BOTH_LEFT_FIRST,
  BOTH_RIGHT_FIRST,
};

enum class buzzer_state_t {
  IDLE,
  DIT,
  DAH,
};

struct cw_ctx
{
  enum paddles_state_t paddles_state = paddles_state_t::IDLE;
  char lcd_output[LCD_OUTPUT_SIZE];
  unsigned lcd_offset = 0;
};

static TickType_t ms2ticks(unsigned long ms)
{
  return ms / portTICK_PERIOD_MS;
}

static struct cw_ctx ctx;

static char morse_table[MORSE_TABLE_SIZE] = {};

static void init_morse_table(char *morse_table)
{
  morse_table[0b0000101] = 'A';
  morse_table[0b0011000] = 'B';
  morse_table[0b0011010] = 'C';
  morse_table[0b0001100] = 'D';
  morse_table[0b0000010] = 'E';
  morse_table[0b0010010] = 'F';
  morse_table[0b0001110] = 'G';
  morse_table[0b0010000] = 'H';
  morse_table[0b0000100] = 'I';
  morse_table[0b0010111] = 'J';
  morse_table[0b0001101] = 'K';
  morse_table[0b0010100] = 'L';
  morse_table[0b0000111] = 'M';
  morse_table[0b0000110] = 'N';
  morse_table[0b0001111] = 'O';
  morse_table[0b0010110] = 'P';
  morse_table[0b0011101] = 'Q';
  morse_table[0b0001010] = 'R';
  morse_table[0b0001000] = 'S';
  morse_table[0b0000011] = 'T';
  morse_table[0b0001001] = 'U';
  morse_table[0b0010001] = 'V';
  morse_table[0b0001011] = 'W';
  morse_table[0b0011001] = 'X';
  morse_table[0b0011011] = 'Y';
  morse_table[0b0011100] = 'Z';
  morse_table[0b0111111] = '0';
  morse_table[0b0101111] = '1';
  morse_table[0b0100111] = '2';
  morse_table[0b0100011] = '3';
  morse_table[0b0100001] = '4';
  morse_table[0b0100000] = '5';
  morse_table[0b0110000] = '6';
  morse_table[0b0111000] = '7';
  morse_table[0b0111100] = '8';
  morse_table[0b0111110] = '9';
}

void setup()
{
  Serial.begin(9600);
  while (!Serial) {
  }

  init_morse_table(morse_table);

  xTaskCreate(task_update_lcd , "update_lcd", 128, &ctx, 6, NULL);
  xTaskCreate(task_cw , "cw", 128, &ctx, 6, NULL);
  xTaskCreate(task_check_paddles , "check_paddles", 128, &ctx, 6, NULL);
}

void loop()
{
  // empty
}

void task_update_lcd(void *args)
{
  struct cw_ctx *ctx = (struct cw_ctx *)args;
  
  LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
  
  for (int i = 0; i < LCD_OUTPUT_SIZE; i++) {
    ctx->lcd_output[i] = ' ';
  }
  
  lcd.begin(LCD_WIDTH, LCD_HEIGHT);
  lcd.print("Morse Code Decoder");
  lcd.cursor();

  while (true) {
    lcd.setCursor(0, 1);
    for (uint16_t offset = (ctx->lcd_offset + 1) % LCD_OUTPUT_SIZE;
         offset != ctx->lcd_offset;
         offset = (offset + 1) % LCD_OUTPUT_SIZE) {
      lcd.print(ctx->lcd_output[offset]);
    }
    vTaskDelay(ms2ticks(50));
  }
}

static void play_dit(uint16_t buzzer_pin)
{
  tone(buzzer_pin, TONE_FREQ);
  vTaskDelay(ms2ticks(dit_ms));
  noTone(buzzer_pin);
  vTaskDelay(ms2ticks(dit_ms));
}

static void play_dah(uint16_t buzzer_pin)
{
  tone(buzzer_pin, TONE_FREQ);
  vTaskDelay(ms2ticks(3 * dit_ms));
  noTone(buzzer_pin);
  vTaskDelay(ms2ticks(dit_ms));
}

static void append_char_real(struct cw_ctx *ctx, char c)
{
  ctx->lcd_output[ctx->lcd_offset] = c;
  ctx->lcd_offset = (ctx->lcd_offset + 1) % LCD_OUTPUT_SIZE;
}

static void append_string(struct cw_ctx *ctx, const char *c)
{
  while (*c) {
    append_char_real(ctx, *c);
    c++;
  }
}

static void append_char(struct cw_ctx *ctx, char c)
{
  if (c == '\0') {
    append_string(ctx, "<0>");
  } else {
    append_char_real(ctx, c);
  }
}

static void task_cw(void *args)
{
  struct cw_ctx *ctx = (struct cw_ctx *)args;
  uint16_t buzzer_pin = BUZZER;
  enum buzzer_state_t buzzer_state = buzzer_state_t::IDLE;
  unsigned long last_buzzer_ms = 0;
  bool output_done = true;
  bool space_done = true;

  pinMode(buzzer_pin, OUTPUT);

  uint16_t code = 0b1;
  while (true) {
    switch (ctx->paddles_state) {
    case paddles_state_t::IDLE:
      buzzer_state = buzzer_state_t::IDLE;
      break;

    case paddles_state_t::LEFT:
      buzzer_state = buzzer_state_t::DIT;
      break;

    case paddles_state_t::RIGHT:
      buzzer_state = buzzer_state_t::DAH;
      break;

    case paddles_state_t::BOTH_LEFT_FIRST:
    case paddles_state_t::BOTH_RIGHT_FIRST:
      if (buzzer_state == buzzer_state_t::DIT) {
        buzzer_state = buzzer_state_t::DAH;
      } else if (buzzer_state == buzzer_state_t::DAH) {
        buzzer_state = buzzer_state_t::DIT;
      } else if (ctx->paddles_state == paddles_state_t::BOTH_LEFT_FIRST) {
        buzzer_state = buzzer_state_t::DIT;
      } else if (ctx->paddles_state == paddles_state_t::BOTH_RIGHT_FIRST) {
        buzzer_state = buzzer_state_t::DAH;
      }
      break;

    default:
      panic();
      break;
    }

    switch (buzzer_state) {
    case buzzer_state_t::DIT:
      output_done = false;
      space_done = false;
      code = (code << 1) | 0b0;
      play_dit(buzzer_pin);
      last_buzzer_ms = millis();
      break;

    case buzzer_state_t::DAH:
      output_done = false;
      space_done = false;
      code = (code << 1) | 0b1;
      play_dah(buzzer_pin);
      last_buzzer_ms = millis();
      break;

    case buzzer_state_t::IDLE:
    default:
      unsigned long interval_ms = millis() - last_buzzer_ms;
      if (!output_done && interval_ms > dit_ms) {
        output_done = true;
        char c = '\0';
        if (code < MORSE_TABLE_SIZE) {
          c = morse_table[code];
        }
        append_char(ctx, c);
        code = 0b1;
      }

      if (!space_done && interval_ms > 4 * dit_ms) {
        space_done = true;
        append_char(ctx, ' ');
      }
      // continue
      break;
    }
    taskYIELD();
  }
}

void task_check_paddles(void *args)
{
  struct cw_ctx *ctx = (struct cw_ctx *)args;
  
  pinMode(PADDLE_LEFT, INPUT_PULLUP);
  pinMode(PADDLE_RIGHT, INPUT_PULLUP);

  while (true) {
    bool left = (digitalRead(PADDLE_LEFT) == LOW);
    bool right = (digitalRead(PADDLE_RIGHT) == LOW);
    if (!left && !right) {
      ctx->paddles_state = paddles_state_t::IDLE;
    } else if (left && right) {
      if (ctx->paddles_state == paddles_state_t::RIGHT) {
        ctx->paddles_state = paddles_state_t::BOTH_RIGHT_FIRST;
      } else if (ctx->paddles_state == paddles_state_t::LEFT ||
                 ctx->paddles_state == paddles_state_t::IDLE) {
        ctx->paddles_state = paddles_state_t::BOTH_LEFT_FIRST;
      } else {
        // keep current state
      }
    } else if (left) {
      ctx->paddles_state = paddles_state_t::LEFT;
    } else if (right) {
      ctx->paddles_state = paddles_state_t::RIGHT;
    } else {
      panic();
    }
    taskYIELD();
  }
}
