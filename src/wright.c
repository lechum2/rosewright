#include <pebble.h>

#include "hand_table.h"
#include "../resources/generated_config.h"
#include "../resources/generated_table.c"
#include "bluetooth_indicator.h"
#include "battery_gauge.h"
#include "config_options.h"

#define SECONDS_PER_DAY 86400
#define SECONDS_PER_HOUR 3600
#define MS_PER_DAY (SECONDS_PER_DAY * 1000)

#define SCREEN_WIDTH 144
#define SCREEN_HEIGHT 168

Window *window;

GBitmap *clock_face_bitmap;
BitmapLayer *clock_face_layer;

GBitmap *chrono_dial_tenths_bitmap_white;
GBitmap *chrono_dial_tenths_bitmap_black;
GBitmap *chrono_dial_hours_bitmap_white;
GBitmap *chrono_dial_hours_bitmap_black;
Layer *chrono_dial_layer;

// Number of laps preserved for the laps digital display
#define CHRONO_MAX_LAPS 4

// This window is pushed on top of the chrono dial to display the
// readout in digital form for ease of recording.
Window *chrono_digital_window;
TextLayer *chrono_digital_current_layer = NULL;
TextLayer *chrono_digital_laps_layer[CHRONO_MAX_LAPS];
bool chrono_digital_window_showing = false;
AppTimer *chrono_digital_timer = NULL;
#define CHRONO_DIGITAL_BUFFER_SIZE 48
char chrono_current_buffer[CHRONO_DIGITAL_BUFFER_SIZE];
char chrono_laps_buffer[CHRONO_MAX_LAPS][CHRONO_DIGITAL_BUFFER_SIZE] = { "ab", "cd", "ef", "gh" };

#define CHRONO_DIGITAL_TICK_MS 100 // Every 0.1 seconds

// True if we're currently showing tenths, false if we're currently
// showing hours, in the chrono subdial.
bool chrono_dial_shows_tenths = true;

// Triggered at regular intervals to implement sweep seconds.
AppTimer *sweep_timer = NULL;
int sweep_timer_ms = 1000;

int sweep_seconds_ms = 60 * 1000 / NUM_STEPS_SECOND;
int sweep_chrono_seconds_ms = 60 * 1000 / NUM_STEPS_CHRONO_SECOND;

Layer *hour_layer;
Layer *minute_layer;
Layer *second_layer;
Layer *chrono_minute_layer;
Layer *chrono_second_layer;
Layer *chrono_tenth_layer;

Layer *day_layer;  // day of the week (abbr)
Layer *date_layer; // numeric date of the month

// This structure keeps track of the things that change on the visible
// watch face and their current state.
struct HandPlacement {
  int hour_hand_index;
  int minute_hand_index;
  int second_hand_index;
  int chrono_minute_hand_index;
  int chrono_second_hand_index;
  int chrono_tenth_hand_index;
  int day_index;
  int date_value;

  // Not really a hand placement, but this is used to keep track of
  // whether we have buzzed for the top of the hour or not.
  int hour_buzzer;
};

struct HandPlacement current_placement;

typedef struct {
  GCompOp paint_black;
  GCompOp paint_white;
  GCompOp paint_assign;
  GColor colors[3];
} DrawModeTable;

DrawModeTable draw_mode_table[2] = {
  { GCompOpClear, GCompOpOr, GCompOpAssign, { GColorClear, GColorBlack, GColorWhite } },
  { GCompOpOr, GCompOpClear, GCompOpAssignInverted, { GColorClear, GColorWhite, GColorBlack } },
};

static const uint32_t tap_segments[] = { 50 };
VibePattern tap = {
  tap_segments,
  1,
};

int stacking_order[] = {
STACKING_ORDER_LIST
};

typedef struct {
  int start_ms;              // consulted if chrono_data.running && !chrono_data.lap_paused
  int hold_ms;               // consulted if !chrono_data.running || chrono_data.lap_paused
  unsigned char running;              // the chronograph has been started
  unsigned char lap_paused;           // the "lap" button has been pressed
  int laps[CHRONO_MAX_LAPS];
} __attribute__((__packed__)) ChronoData;

ChronoData chrono_data = { false, false, 0, 0, { 0, 0, 0, 0 } };

// Returns the number of milliseconds since midnight.
int get_time_ms(struct tm *time) {
  time_t s;
  uint16_t ms;
  int result;

  time_ms(&s, &ms);
  result = (s % SECONDS_PER_DAY) * 1000 + ms;

#ifdef FAST_TIME
  if (time != NULL) {
    time->tm_wday = s % 7;
    time->tm_mday = (s % 31) + 1;
  }
  result *= 67;
#endif  // FAST_TIME

  return result;
}

// Returns the time showing on the chronograph, given the ms returned
// by get_time_ms().  Returns the current lap time if the lap is
// paused.
int get_chrono_ms(int ms) {
  int chrono_ms;
  if (chrono_data.running && !chrono_data.lap_paused) {
    // The chronograph is running.  Show the active elapsed time.
    chrono_ms = (ms - chrono_data.start_ms + MS_PER_DAY) % MS_PER_DAY;
  } else {
    // The chronograph is paused.  Show the time it is paused on.
    chrono_ms = chrono_data.hold_ms;
  }

  return chrono_ms;
}

// Returns the time showing on the chronograph, given the ms returned
// by get_time_ms().  Never returns the current lap time.
int get_chrono_ms_no_lap(int ms) {
  int chrono_ms;
  if (chrono_data.running) {
    // The chronograph is running.  Show the active elapsed time.
    chrono_ms = (ms - chrono_data.start_ms + MS_PER_DAY) % MS_PER_DAY;
  } else {
    // The chronograph is paused.  Show the time it is paused on.
    chrono_ms = chrono_data.hold_ms;
  }

  return chrono_ms;
}
  
// Determines the specific hand bitmaps that should be displayed based
// on the current time.
void compute_hands(struct tm *time, struct HandPlacement *placement) {
  int ms;

  ms = get_time_ms(time);

  placement->hour_hand_index = ((NUM_STEPS_HOUR * ms) / (SECONDS_PER_HOUR * 12 * 1000)) % NUM_STEPS_HOUR;
  placement->minute_hand_index = ((NUM_STEPS_MINUTE * ms) / (SECONDS_PER_HOUR * 1000)) % NUM_STEPS_MINUTE;
  placement->second_hand_index = ((NUM_STEPS_SECOND * ms) / (60 * 1000)) % NUM_STEPS_SECOND;

#ifdef SHOW_DAY_CARD
  if (time != NULL) {
    placement->day_index = time->tm_wday;
  }
#endif  // SHOW_DAY_CARD

#ifdef SHOW_DATE_CARD
  if (time != NULL) {
    placement->date_value = time->tm_mday;
  }
#endif  // SHOW_DATE_CARD

  placement->hour_buzzer = (ms / (SECONDS_PER_HOUR * 1000)) % 24;

#ifdef MAKE_CHRONOGRAPH
  {
    int chrono_ms = get_chrono_ms(ms);

    bool chrono_dial_wants_tenths = true;
    switch (config.chrono_dial) {
    case CDM_off:
      break;

    case CDM_tenths:
      chrono_dial_wants_tenths = true;
      break;

    case CDM_hours:
      chrono_dial_wants_tenths = false;
      break;

    case CDM_dual:
      // In dual mode, we show either tenths or hours, depending on the
      // amount of elapsed time.  Less than 30 minutes shows tenths.
      chrono_dial_wants_tenths = (chrono_ms < 30 * 60 * 1000);
      break;
    }

    if (chrono_dial_shows_tenths != chrono_dial_wants_tenths) {
      // The dial has changed states; redraw it.
      chrono_dial_shows_tenths = chrono_dial_wants_tenths;
      if (chrono_dial_layer != NULL) {
	layer_mark_dirty(chrono_dial_layer);
      }
    }
    
#ifdef SHOW_CHRONO_MINUTE_HAND
    // The chronograph minute hand rolls completely around in 30
    // minutes (not 60).
    placement->chrono_minute_hand_index = ((NUM_STEPS_CHRONO_MINUTE * chrono_ms) / (1800 * 1000)) % NUM_STEPS_CHRONO_MINUTE;
#endif  // SHOW_CHRONO_MINUTE_HAND

#ifdef SHOW_CHRONO_SECOND_HAND
    placement->chrono_second_hand_index = ((NUM_STEPS_CHRONO_SECOND * chrono_ms) / (60 * 1000)) % NUM_STEPS_CHRONO_SECOND;
#endif  // SHOW_CHRONO_SECOND_HAND

#ifdef SHOW_CHRONO_TENTH_HAND
    if (chrono_dial_shows_tenths) {
      // Drawing tenths-of-a-second.
      if (chrono_data.running && !chrono_data.lap_paused) {
	// We don't actually show the tenths time while the chrono is running.
	placement->chrono_tenth_hand_index = 0;
      } else {
	// We show the tenths time when the chrono is stopped or showing
	// the lap time.
	placement->chrono_tenth_hand_index = ((NUM_STEPS_CHRONO_TENTH * chrono_ms) / (100)) % NUM_STEPS_CHRONO_TENTH;
      }
    } else {
      // Drawing hours.  12-hour scale.
      placement->chrono_tenth_hand_index = ((NUM_STEPS_CHRONO_TENTH * chrono_ms) / (12 * SECONDS_PER_HOUR * 1000)) % NUM_STEPS_CHRONO_TENTH;
    }
#endif  // SHOW_CHRONO_TENTH_HAND

  }
#endif  // MAKE_CHRONOGRAPH
}


#define MAX_DIGITS 12
const char *quick_itoa(int value) {
  int neg = 0;
  static char digits[MAX_DIGITS + 2];
  char *p = digits + MAX_DIGITS + 1;
  *p = '\0';

  if (value < 0) {
    value = -value;
    neg = 1;
  }

  do {
    --p;
    if (p < digits) {
      digits[0] = '*';
      return digits;
    }

    *p = '0' + (value % 10);
    value /= 10;
  } while (value != 0);

  if (neg) {
    --p;
    if (p < digits) {
      digits[0] = '*';
      return digits;
    }

    *p = '-';
  }

  return p;
}

// Reverse the bits of a byte.
// http://www-graphics.stanford.edu/~seander/bithacks.html#BitReverseTable
uint8_t reverse_bits(uint8_t b) {
  return ((b * 0x0802LU & 0x22110LU) | (b * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16; 
}

// Horizontally flips the indicated GBitmap in-place.  Requires
// that the width be a multiple of 8 pixels.
void flip_bitmap_x(GBitmap *image, int *cx) {
  int height = image->bounds.size.h;
  int width = image->bounds.size.w;  // multiple of 8, by our convention.
  int width_bytes = width / 8;
  int stride = image->row_size_bytes; // multiple of 4, by Pebble.
  uint8_t *data = image->addr;

  for (int y = 0; y < height; ++y) {
    uint8_t *row = data + y * stride;
    for (int x1 = (width_bytes - 1) / 2; x1 >= 0; --x1) {
      int x2 = width_bytes - 1 - x1;
      uint8_t b = reverse_bits(row[x1]);
      row[x1] = reverse_bits(row[x2]);
      row[x2] = b;
    }
  }

  if (cx != NULL) {
    *cx = width- 1 - *cx;
  }
}

// Vertically flips the indicated GBitmap in-place.
void flip_bitmap_y(GBitmap *image, int *cy) {
  int height = image->bounds.size.h;
  int stride = image->row_size_bytes; // multiple of 4.
  uint8_t *data = image->addr;

#if 1
  /* This is the slightly slower flip, that requires less RAM on the
     stack. */
  uint8_t buffer[stride]; // gcc lets us do this.
  for (int y1 = (height - 1) / 2; y1 >= 0; --y1) {
    int y2 = height - 1 - y1;
    // Swap rows y1 and y2.
    memcpy(buffer, data + y1 * stride, stride);
    memcpy(data + y1 * stride, data + y2 * stride, stride);
    memcpy(data + y2 * stride, buffer, stride);
  }

#else
  /* This is the slightly faster flip, that requires more RAM on the
     stack.  I have no idea what our stack limit is on the Pebble, or
     what happens if we exceed it. */
  uint8_t buffer[height * stride]; // gcc lets us do this.
  memcpy(buffer, data, height * stride);
  for (int y1 = 0; y1 < height; ++y1) {
    int y2 = height - 1 - y1;
    memcpy(data + y1 * stride, buffer + y2 * stride, stride);
  }
#endif

  if (cy != NULL) {
    *cy = height - 1 - *cy;
  }
}

// Draws a given hand on the face, using the vector structures.
void draw_vector_hand(struct VectorHandTable *hand, int hand_index, int num_steps,
                      int place_x, int place_y, GContext *ctx) {
  GPoint center = { place_x, place_y };
  int32_t angle = TRIG_MAX_ANGLE * hand_index / num_steps;
  int gi;

  for (gi = 0; gi < hand->num_groups; ++gi) {
    struct VectorHandGroup *group = &hand->group[gi];

    GPath *path = gpath_create(&group->path_info);

    gpath_rotate_to(path, angle);
    gpath_move_to(path, center);

    if (group->fill != GColorClear) {
      graphics_context_set_fill_color(ctx, draw_mode_table[config.draw_mode].colors[group->fill]);
      gpath_draw_filled(ctx, path);
    }
    if (group->outline != GColorClear) {
      graphics_context_set_stroke_color(ctx, draw_mode_table[config.draw_mode].colors[group->outline]);
      gpath_draw_outline(ctx, path);
    }

    gpath_destroy(path);
  }
}

// Draws a given hand on the face, using the bitmap structures.
void draw_bitmap_hand(struct BitmapHandTableRow *hand, int place_x, int place_y, GContext *ctx) {
  int cx, cy;
  cx = hand->cx;
  cy = hand->cy;

  if (hand->mask_id == hand->image_id) {
    // The hand does not have a mask.  Draw the hand on top of the scene.
    GBitmap *image;
    image = gbitmap_create_with_resource(hand->image_id);
    
    if (hand->flip_x) {
      // To minimize wasteful resource usage, if the hand is symmetric
      // we can store only the bitmaps for the right half of the clock
      // face, and flip them for the left half.
      flip_bitmap_x(image, &cx);
    }
    
    if (hand->flip_y) {
      // We can also do this vertically.
      flip_bitmap_y(image, &cy);
    }
    
    // We make sure the dimensions of the GRect to draw into
    // are equal to the size of the bitmap--otherwise the image
    // will automatically tile.
    GRect destination = image->bounds;
    
    // Place the hand's center point at place_x, place_y.
    destination.origin.x = place_x - cx;
    destination.origin.y = place_y - cy;
    
    // Specify a compositing mode to make the hands overlay on top of
    // each other, instead of the background parts of the bitmaps
    // blocking each other.

    if (hand->paint_black) {
      // Painting foreground ("white") pixels as black.
      graphics_context_set_compositing_mode(ctx, draw_mode_table[config.draw_mode].paint_black);
    } else {
      // Painting foreground ("white") pixels as white.
      graphics_context_set_compositing_mode(ctx, draw_mode_table[config.draw_mode].paint_white);
    }
      
    graphics_draw_bitmap_in_rect(ctx, image, destination);
    
    gbitmap_destroy(image);

  } else {
    // The hand has a mask, so use it to draw the hand opaquely.
    GBitmap *image, *mask;
    image = gbitmap_create_with_resource(hand->image_id);
    mask = gbitmap_create_with_resource(hand->mask_id);
    
    if (hand->flip_x) {
      // To minimize wasteful resource usage, if the hand is symmetric
      // we can store only the bitmaps for the right half of the clock
      // face, and flip them for the left half.
      flip_bitmap_x(image, &cx);
      flip_bitmap_x(mask, NULL);
    }
    
    if (hand->flip_y) {
      // We can also do this vertically.
      flip_bitmap_y(image, &cy);
      flip_bitmap_y(mask, NULL);
    }
    
    GRect destination = image->bounds;
    
    destination.origin.x = place_x - cx;
    destination.origin.y = place_y - cy;

    graphics_context_set_compositing_mode(ctx, draw_mode_table[config.draw_mode].paint_white);
    graphics_draw_bitmap_in_rect(ctx, mask, destination);
    
    graphics_context_set_compositing_mode(ctx, draw_mode_table[config.draw_mode].paint_black);
    graphics_draw_bitmap_in_rect(ctx, image, destination);
    
    gbitmap_destroy(image);
    gbitmap_destroy(mask);
  }
}
  
void hour_layer_update_callback(Layer *me, GContext *ctx) {
  (void)me;

#ifdef VECTOR_HOUR_HAND
  draw_vector_hand(&hour_hand_vector_table, current_placement.hour_hand_index,
                   NUM_STEPS_HOUR, HOUR_HAND_X, HOUR_HAND_Y, ctx);
#endif

#ifdef BITMAP_HOUR_HAND
  draw_bitmap_hand(&hour_hand_bitmap_table[current_placement.hour_hand_index],
                   HOUR_HAND_X, HOUR_HAND_Y, ctx);
#endif
}

void minute_layer_update_callback(Layer *me, GContext *ctx) {
  (void)me;

#ifdef VECTOR_MINUTE_HAND
  draw_vector_hand(&minute_hand_vector_table, current_placement.minute_hand_index,
                   NUM_STEPS_MINUTE, MINUTE_HAND_X, MINUTE_HAND_Y, ctx);
#endif

#ifdef BITMAP_MINUTE_HAND
  draw_bitmap_hand(&minute_hand_bitmap_table[current_placement.minute_hand_index],
                   MINUTE_HAND_X, MINUTE_HAND_Y, ctx);
#endif
}

void second_layer_update_callback(Layer *me, GContext *ctx) {
  (void)me;

  if (config.second_hand) {
#ifdef VECTOR_SECOND_HAND
    draw_vector_hand(&second_hand_vector_table, current_placement.second_hand_index,
		     NUM_STEPS_SECOND, SECOND_HAND_X, SECOND_HAND_Y, ctx);
#endif
    
#ifdef BITMAP_SECOND_HAND
    draw_bitmap_hand(&second_hand_bitmap_table[current_placement.second_hand_index],
		     SECOND_HAND_X, SECOND_HAND_Y, ctx);
#endif
  }
}

#ifdef SHOW_CHRONO_MINUTE_HAND
void chrono_minute_layer_update_callback(Layer *me, GContext *ctx) {
  (void)me;

  if (config.second_hand || chrono_data.running || chrono_data.hold_ms != 0) {
#ifdef VECTOR_CHRONO_MINUTE_HAND
    draw_vector_hand(&chrono_minute_hand_vector_table, current_placement.chrono_minute_hand_index,
		     NUM_STEPS_CHRONO_MINUTE, CHRONO_MINUTE_HAND_X, CHRONO_MINUTE_HAND_Y, ctx);
#endif
    
#ifdef BITMAP_CHRONO_MINUTE_HAND
    draw_bitmap_hand(&chrono_minute_hand_bitmap_table[current_placement.chrono_minute_hand_index],
		     CHRONO_MINUTE_HAND_X, CHRONO_MINUTE_HAND_Y, ctx);
#endif
  }
}
#endif  // SHOW_CHRONO_MINUTE_HAND

#ifdef SHOW_CHRONO_SECOND_HAND
void chrono_second_layer_update_callback(Layer *me, GContext *ctx) {
  (void)me;

  if (config.second_hand || chrono_data.running || chrono_data.hold_ms != 0) {
#ifdef VECTOR_CHRONO_SECOND_HAND
    draw_vector_hand(&chrono_second_hand_vector_table, current_placement.chrono_second_hand_index,
		     NUM_STEPS_CHRONO_SECOND, CHRONO_SECOND_HAND_X, CHRONO_SECOND_HAND_Y, ctx);
#endif
    
#ifdef BITMAP_CHRONO_SECOND_HAND
    draw_bitmap_hand(&chrono_second_hand_bitmap_table[current_placement.chrono_second_hand_index],
		     CHRONO_SECOND_HAND_X, CHRONO_SECOND_HAND_Y, ctx);
#endif
  }
}
#endif  // SHOW_CHRONO_SECOND_HAND

#ifdef SHOW_CHRONO_TENTH_HAND
void chrono_tenth_layer_update_callback(Layer *me, GContext *ctx) {
  (void)me;

  if (config.second_hand || chrono_data.running || chrono_data.hold_ms != 0) {
#ifdef VECTOR_CHRONO_TENTH_HAND
    draw_vector_hand(&chrono_tenth_hand_vector_table, current_placement.chrono_tenth_hand_index,
		     NUM_STEPS_CHRONO_TENTH, CHRONO_TENTH_HAND_X, CHRONO_TENTH_HAND_Y, ctx);
#endif
    
#ifdef BITMAP_CHRONO_TENTH_HAND
    draw_bitmap_hand(&chrono_tenth_hand_bitmap_table[current_placement.chrono_tenth_hand_index],
		     CHRONO_TENTH_HAND_X, CHRONO_TENTH_HAND_Y, ctx);
#endif
  }
}
#endif  // SHOW_CHRONO_TENTH_HAND

void draw_card(Layer *me, GContext *ctx, const char *text, bool on_black, bool bold) {
  GFont font;
  GRect box;

  if (bold) {
    font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  } else {
    font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  }
  box = layer_get_frame(me);
  box.origin.x = 0;
  box.origin.y = 0;
  if (on_black) {
    graphics_context_set_text_color(ctx, GColorWhite);
  } else {
    graphics_context_set_text_color(ctx, GColorBlack);
  }

  box.origin.y -= 3;  // Determined empirically.

  graphics_draw_text(ctx, text, font, box,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     NULL);
}

#ifdef MAKE_CHRONOGRAPH
void chrono_dial_layer_update_callback(Layer *me, GContext *ctx) {
  if (config.chrono_dial != CDM_off) {
    GRect destination = layer_get_bounds(me);
    destination.origin.x = 0;
    destination.origin.y = 0;

    if (chrono_dial_shows_tenths) {
      // Draw the tenths dial.
      graphics_context_set_compositing_mode(ctx, draw_mode_table[config.draw_mode].paint_black);
      graphics_draw_bitmap_in_rect(ctx, chrono_dial_tenths_bitmap_black, destination);
      graphics_context_set_compositing_mode(ctx, draw_mode_table[config.draw_mode].paint_white);
      graphics_draw_bitmap_in_rect(ctx, chrono_dial_tenths_bitmap_white, destination);
    } else {
      // Draw the hours dial.
      graphics_context_set_compositing_mode(ctx, draw_mode_table[config.draw_mode].paint_black);
      graphics_draw_bitmap_in_rect(ctx, chrono_dial_hours_bitmap_black, destination);
      graphics_context_set_compositing_mode(ctx, draw_mode_table[config.draw_mode].paint_white);
      graphics_draw_bitmap_in_rect(ctx, chrono_dial_hours_bitmap_white, destination);
    }
  }
}
#endif  // MAKE_CHRONOGRAPH

/*
#ifdef MAKE_CHRONOGRAPH
void chrono_digital_current_layer_update_callback(Layer *me, GContext *ctx) {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "chrono_digital_current_layer_update_callback: %p, %p", me, ctx);

  GRect destination = layer_get_bounds(me);
  destination.origin.x = 0;
  destination.origin.y = 0;
}
#endif  // MAKE_CHRONOGRAPH
*/

#ifdef SHOW_DAY_CARD
void day_layer_update_callback(Layer *me, GContext *ctx) {
  draw_card(me, ctx, weekday_names[current_placement.day_index], (DAY_CARD_ON_BLACK) ^ config.draw_mode, DAY_CARD_BOLD);
}
#endif  // SHOW_DAY_CARD

#ifdef SHOW_DATE_CARD
void date_layer_update_callback(Layer *me, GContext *ctx) {
  draw_card(me, ctx, quick_itoa(current_placement.date_value), (DATE_CARD_ON_BLACK) ^ config.draw_mode, DATE_CARD_BOLD);
}
#endif  // SHOW_DATE_CARD

void update_hands(struct tm *time) {
  struct HandPlacement new_placement;

  compute_hands(time, &new_placement);
  if (new_placement.hour_hand_index != current_placement.hour_hand_index) {
    current_placement.hour_hand_index = new_placement.hour_hand_index;
    layer_mark_dirty(hour_layer);
  }

  if (new_placement.minute_hand_index != current_placement.minute_hand_index) {
    current_placement.minute_hand_index = new_placement.minute_hand_index;
    layer_mark_dirty(minute_layer);
  }

  if (new_placement.second_hand_index != current_placement.second_hand_index) {
    current_placement.second_hand_index = new_placement.second_hand_index;
    layer_mark_dirty(second_layer);
  }

  if (new_placement.hour_buzzer != current_placement.hour_buzzer) {
    current_placement.hour_buzzer = new_placement.hour_buzzer;
    if (config.hour_buzzer) {
      vibes_short_pulse();
    }
  }

  // Make sure the sweep timer is fast enough to capture the second
  // hand.
  sweep_timer_ms = sweep_seconds_ms;

#ifdef MAKE_CHRONOGRAPH

#ifdef SHOW_CHRONO_MINUTE_HAND
  if (new_placement.chrono_minute_hand_index != current_placement.chrono_minute_hand_index) {
    current_placement.chrono_minute_hand_index = new_placement.chrono_minute_hand_index;
    layer_mark_dirty(chrono_minute_layer);
  }
#endif  // SHOW_CHRONO_MINUTE_HAND

#ifdef SHOW_CHRONO_SECOND_HAND
  if (new_placement.chrono_second_hand_index != current_placement.chrono_second_hand_index) {
    current_placement.chrono_second_hand_index = new_placement.chrono_second_hand_index;
    layer_mark_dirty(chrono_second_layer);
  }
#endif  // SHOW_CHRONO_SECOND_HAND

#ifdef SHOW_CHRONO_TENTH_HAND
  if (new_placement.chrono_tenth_hand_index != current_placement.chrono_tenth_hand_index) {
    current_placement.chrono_tenth_hand_index = new_placement.chrono_tenth_hand_index;
    layer_mark_dirty(chrono_tenth_layer);
  }
#endif  // SHOW_CHRONO_TENTH_HAND

  if (chrono_data.running && !chrono_data.lap_paused && !chrono_digital_window_showing) {
    // With the chronograph running, the sweep timer must be fast
    // enough to capture the chrono second hand.
    if (sweep_chrono_seconds_ms < sweep_timer_ms) {
      sweep_timer_ms = sweep_chrono_seconds_ms;
    }
  }

#endif  // MAKE_CHRONOGRAPH

#ifdef SHOW_DAY_CARD
  if (new_placement.day_index != current_placement.day_index) {
    current_placement.day_index = new_placement.day_index;
    layer_mark_dirty(day_layer);
  }
#endif  // SHOW_DAY_CARD

#ifdef SHOW_DATE_CARD
  if (new_placement.date_value != current_placement.date_value) {
    current_placement.date_value = new_placement.date_value;
    layer_mark_dirty(date_layer);
  }
#endif  // SHOW_DATE_CARD
}

// Triggered at sweep_timer_ms intervals to run the sweep-second hand.
void handle_sweep(void *data) {
  sweep_timer = NULL;  // When the timer is handled, it is implicitly canceled.
  if (sweep_timer_ms < 1000) {
    update_hands(NULL);
    sweep_timer = app_timer_register(sweep_timer_ms, &handle_sweep, 0);
  }
}

void reset_sweep() {
  if (sweep_timer != NULL) {
    app_timer_cancel(sweep_timer);
    sweep_timer = NULL;
  }
  if (sweep_timer_ms < 1000) {
    sweep_timer = app_timer_register(sweep_timer_ms, &handle_sweep, 0);
  }
}

// Compute new hand positions once a minute (or once a second).
void handle_tick(struct tm *tick_time, TimeUnits units_changed) {
  update_hands(tick_time);
  reset_sweep();
}

// Forward references.
void stopped_click_config_provider(void *context);
void started_click_config_provider(void *context);
void reset_chrono_digital_timer();
void record_chrono_lap(int chrono_ms);
void update_chrono_laps_time();

void chrono_start_stop_handler(ClickRecognizerRef recognizer, void *context) {
  Window *window = (Window *)context;
  int ms = get_time_ms(NULL);

  // The start/stop button was pressed.
  if (chrono_data.running) {
    // If the chronograph is currently running, this means to stop (or
    // pause).
    chrono_data.hold_ms = ms - chrono_data.start_ms;
    chrono_data.running = false;
    chrono_data.lap_paused = false;
    vibes_enqueue_custom_pattern(tap);
    update_hands(NULL);
    apply_config();

    // We change the click config provider according to the chrono run
    // state.  When the chrono is stopped, we listen for a different
    // set of buttons than when it is started.
    window_set_click_config_provider(window, &stopped_click_config_provider);
    window_set_click_config_provider(chrono_digital_window, &stopped_click_config_provider);
  } else {
    // If the chronograph is not currently running, this means to
    // start, from the currently showing Chronograph time.
    chrono_data.start_ms = ms - chrono_data.hold_ms;
    chrono_data.running = true;
    if (sweep_chrono_seconds_ms < sweep_timer_ms) {
      sweep_timer_ms = sweep_chrono_seconds_ms;
    }
    vibes_enqueue_custom_pattern(tap);
    update_hands(NULL);
    apply_config();

    window_set_click_config_provider(window, &started_click_config_provider);
    window_set_click_config_provider(chrono_digital_window, &started_click_config_provider);
  }
}

void chrono_lap_button() {
  int ms;
 
  ms = get_time_ms(NULL);

  if (chrono_data.lap_paused) {
    // If we were already paused, this resumes the motion, jumping
    // ahead to the currently elapsed time.
    chrono_data.lap_paused = false;
    vibes_enqueue_custom_pattern(tap);
    update_hands(NULL);
  } else {
    // If we were not already paused, this pauses the hands here (but
    // does not stop the timer).
    int lap_ms = ms - chrono_data.start_ms;
    record_chrono_lap(lap_ms);
    if (!chrono_digital_window_showing) {
      // Actually, we only pause the hands if we're not looking at the
      // digital timer.
      chrono_data.hold_ms = lap_ms;
      chrono_data.lap_paused = true;
    }
    vibes_enqueue_custom_pattern(tap);
    update_hands(NULL);
  }
}

void chrono_reset_button() {
  // Resets the chronometer to 0 time.
  time_t now;
  struct tm *this_time;

  now = time(NULL);
  this_time = localtime(&now);
  chrono_data.running = false;
  chrono_data.lap_paused = false;
  chrono_data.start_ms = 0;
  chrono_data.hold_ms = 0;
  vibes_double_pulse();
  update_chrono_laps_time();
  update_hands(this_time);
  apply_config();
}

void chrono_lap_handler(ClickRecognizerRef recognizer, void *context) {
  // The lap/reset button was pressed (briefly).

  // We only do anything here if the chronograph is currently running.
  if (chrono_data.running) {
    chrono_lap_button();
  }
}

void chrono_lap_or_reset_handler(ClickRecognizerRef recognizer, void *context) {
  // The lap/reset button was pressed (long press).

  // This means a lap if the chronograph is running, and a reset if it
  // is not.
  if (chrono_data.running) {
    chrono_lap_button();
  } else {
    chrono_reset_button();
  }
}

void push_chrono_digital_handler(ClickRecognizerRef recognizer, void *context) {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "push chrono digital");
  if (!chrono_digital_window_showing) {
    window_stack_push(chrono_digital_window, true);
  }
}

// Enable the set of buttons active while the chrono is stopped.
void stopped_click_config_provider(void *context) {
  // single click config:
  window_single_click_subscribe(BUTTON_ID_UP, &chrono_start_stop_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, NULL);

  // long click config:
  window_long_click_subscribe(BUTTON_ID_DOWN, 700, &chrono_lap_or_reset_handler, NULL);

  // To push the digital chrono display.
  window_single_click_subscribe(BUTTON_ID_SELECT, &push_chrono_digital_handler);
}

// Enable the set of buttons active while the chrono is running.
void started_click_config_provider(void *context) {
  // single click config:
  window_single_click_subscribe(BUTTON_ID_UP, &chrono_start_stop_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, &chrono_lap_handler);

  // It's important to disable the lock_click handler while the chrono
  // is running, so that the normal click handler (above) can be
  // immediately responsive.  If we leave the long_click handler
  // active, then the underlying SDK has to wait the full 700 ms to
  // differentiate a long_click from a click, which makes the lap
  // response sluggish.
  window_long_click_subscribe(BUTTON_ID_DOWN, 0, NULL, NULL);

  // To push the digital chrono display.
  window_single_click_subscribe(BUTTON_ID_SELECT, &push_chrono_digital_handler);
}

void window_load_handler(struct Window *window) {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "main window loads");
}

void window_appear_handler(struct Window *window) {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "main window appears");

#ifdef MAKE_CHRONOGRAPH
  if (chrono_data.running) {
    window_set_click_config_provider(window, &started_click_config_provider);
  } else {
    window_set_click_config_provider(window, &stopped_click_config_provider);
  }
#endif  // MAKE_CHRONOGRAPH
}

void window_disappear_handler(struct Window *window) {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "main window disappears");
}

void window_unload_handler(struct Window *window) {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "main window unloads");
}

void update_chrono_laps_time() {
#ifdef MAKE_CHRONOGRAPH
  int i;

  for (i = 0; i < CHRONO_MAX_LAPS; ++i) {
    int chrono_ms = chrono_data.laps[i];
    int chrono_h = chrono_ms / (1000 * 60 * 60);
    int chrono_m = (chrono_ms / (1000 * 60)) % 60;
    int chrono_s = (chrono_ms / (1000)) % 60;
    int chrono_t = (chrono_ms / (100)) % 10;

    if (chrono_ms == 0) {
      // No data: empty string.
      chrono_laps_buffer[i][0] = '\0';
    } else {
      // Real data: formatted string.
      snprintf(chrono_laps_buffer[i], CHRONO_DIGITAL_BUFFER_SIZE, "%d:%02d:%02d.%d", 
	       chrono_h, chrono_m, chrono_s, chrono_t);
    }
    if (chrono_digital_laps_layer[i] != NULL) {
      layer_mark_dirty((Layer *)chrono_digital_laps_layer[i]);
    }
  }
#endif  // MAKE_CHRONOGRAPH
}

#ifdef MAKE_CHRONOGRAPH
void record_chrono_lap(int chrono_ms) {
  // Lose the first one.
  memmove(&chrono_data.laps[0], &chrono_data.laps[1], sizeof(chrono_data.laps[0]) * CHRONO_MAX_LAPS - 1);
  chrono_data.laps[CHRONO_MAX_LAPS - 1] = chrono_ms;
  update_chrono_laps_time();
}

void update_chrono_current_time() {
  int ms = get_time_ms(NULL);
  int chrono_ms = get_chrono_ms_no_lap(ms);
  int chrono_h = chrono_ms / (1000 * 60 * 60);
  int chrono_m = (chrono_ms / (1000 * 60)) % 60;
  int chrono_s = (chrono_ms / (1000)) % 60;
  int chrono_t = (chrono_ms / (100)) % 10;
  
  snprintf(chrono_current_buffer, CHRONO_DIGITAL_BUFFER_SIZE, "%d:%02d:%02d.%d", 
	   chrono_h, chrono_m, chrono_s, chrono_t);
  if (chrono_digital_current_layer != NULL) {
    layer_mark_dirty((Layer *)chrono_digital_current_layer);
  }
}

void handle_chrono_digital_timer(void *data) {
  chrono_digital_timer = NULL;  // When the timer is handled, it is implicitly canceled.

  reset_chrono_digital_timer();
}

void chrono_digital_window_load_handler(struct Window *window) {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "chrono digital loads");

  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);

  Layer *chrono_digital_window_layer = window_get_root_layer(chrono_digital_window);
  chrono_digital_current_layer = text_layer_create(GRect(0, 120, SCREEN_WIDTH, 48));
  int i;
  for (i = 0; i < CHRONO_MAX_LAPS; ++i) {
    chrono_digital_laps_layer[i] = text_layer_create(GRect(0, 30 * i, SCREEN_WIDTH, 30));

    text_layer_set_text(chrono_digital_laps_layer[i], chrono_laps_buffer[i]);
    text_layer_set_text_color(chrono_digital_laps_layer[i], GColorBlack);
    text_layer_set_text_alignment(chrono_digital_laps_layer[i], GTextAlignmentCenter);
    text_layer_set_overflow_mode(chrono_digital_laps_layer[i], GTextOverflowModeFill);
    text_layer_set_font(chrono_digital_laps_layer[i], font);
    layer_add_child(chrono_digital_window_layer, (Layer *)chrono_digital_laps_layer[i]);
  }

  text_layer_set_text(chrono_digital_current_layer, chrono_current_buffer);
  text_layer_set_text_color(chrono_digital_current_layer, GColorBlack);
  text_layer_set_text_alignment(chrono_digital_current_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(chrono_digital_current_layer, GTextOverflowModeFill);
  text_layer_set_font(chrono_digital_current_layer, font);
  layer_add_child(chrono_digital_window_layer, (Layer *)chrono_digital_current_layer);
}

void chrono_digital_window_appear_handler(struct Window *window) {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "chrono digital appears");
  chrono_digital_window_showing = true;

  // We never have the lap timer paused while the digital window is visible.
  chrono_data.lap_paused = false;

  if (chrono_data.running) {
    window_set_click_config_provider(chrono_digital_window, &started_click_config_provider);
  } else {
    window_set_click_config_provider(chrono_digital_window, &stopped_click_config_provider);
  }

  reset_chrono_digital_timer();
}

void chrono_digital_window_disappear_handler(struct Window *window) {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "chrono digital disappears");
  chrono_digital_window_showing = false;
  reset_chrono_digital_timer();
}

void chrono_digital_window_unload_handler(struct Window *window) {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "chrono digital unloads");
  if (chrono_digital_current_layer != NULL) {
    text_layer_destroy(chrono_digital_current_layer);
    chrono_digital_current_layer = NULL;
  }
  int i;
  for (i = 0; i < CHRONO_MAX_LAPS; ++i) {
    if (chrono_digital_laps_layer[i] != NULL) {
      text_layer_destroy(chrono_digital_laps_layer[i]);
      chrono_digital_laps_layer[i] = NULL;
    }
  }
}
#endif  // MAKE_CHRONOGRAPH

void reset_chrono_digital_timer() {
#ifdef MAKE_CHRONOGRAPH
  if (chrono_digital_timer != NULL) {
    app_timer_cancel(chrono_digital_timer);
    chrono_digital_timer = NULL;
  }
  update_chrono_current_time();
  if (chrono_digital_window_showing && chrono_data.running) {
    // Set the timer for the next update.
    chrono_digital_timer = app_timer_register(CHRONO_DIGITAL_TICK_MS, &handle_chrono_digital_timer, 0);
  }
#endif  // MAKE_CHRONOGRAPH
}


// Updates any runtime settings as needed when the config changes.
void apply_config() {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "apply_config, second_hand=%d", config.second_hand);
  tick_timer_service_unsubscribe();

#if defined(FAST_TIME) || defined(BATTERY_HACK)
  tick_timer_service_subscribe(SECOND_UNIT, handle_tick);
#else
  if (config.second_hand || chrono_data.running) {
    tick_timer_service_subscribe(SECOND_UNIT, handle_tick);
  } else {
    tick_timer_service_subscribe(MINUTE_UNIT, handle_tick);
  }
#endif

  // Also adjust the draw mode on the clock_face_layer.  (The other
  // layers all draw themselves interactively.)
  bitmap_layer_set_compositing_mode(clock_face_layer, draw_mode_table[config.draw_mode].paint_assign);
  layer_mark_dirty((Layer *)clock_face_layer);
  reset_chrono_digital_timer();
}

void
load_chrono_data() {
#ifdef MAKE_CHRONOGRAPH
  ChronoData local_data;
  if (persist_read_data(PERSIST_KEY + 0x100, &local_data, sizeof(local_data)) == sizeof(local_data)) {
    chrono_data = local_data;
    app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "Loaded chrono_data");
    update_chrono_laps_time();
  } else {
    app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "Wrong previous chrono_data size or no previous data.");
  }
#endif  // MAKE_CHRONOGRAPH
}

void save_chrono_data() {
#ifdef MAKE_CHRONOGRAPH
  int wrote = persist_write_data(PERSIST_KEY + 0x100, &chrono_data, sizeof(chrono_data));
  if (wrote == sizeof(chrono_data)) {
    app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "Saved chrono_data (%d, %d)", PERSIST_KEY + 0x100, sizeof(chrono_data));
  } else {
    app_log(APP_LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error saving chrono_data (%d, %d): %d", PERSIST_KEY + 0x100, sizeof(chrono_data), wrote);
  }
#endif  // MAKE_CHRONOGRAPH
}

void handle_init() {
  load_config();
  load_chrono_data();

  app_message_register_inbox_received(receive_config_handler);
  app_message_open(96, 96);

  time_t now = time(NULL);
  struct tm *startup_time = localtime(&now);
  int i;

  window = window_create();

  struct WindowHandlers window_handlers;
  memset(&window_handlers, 0, sizeof(window_handlers));
  window_handlers.load = window_load_handler;
  window_handlers.appear = window_appear_handler;
  window_handlers.disappear = window_disappear_handler;
  window_handlers.unload = window_unload_handler;
  window_set_window_handlers(window, window_handlers);

  window_set_fullscreen(window, true);
  window_stack_push(window, true /* Animated */);

  compute_hands(startup_time, &current_placement);

  Layer *window_layer = window_get_root_layer(window);
  GRect window_frame = layer_get_frame(window_layer);

  clock_face_bitmap = gbitmap_create_with_resource(RESOURCE_ID_CLOCK_FACE);
  clock_face_layer = bitmap_layer_create(window_frame);
  bitmap_layer_set_bitmap(clock_face_layer, clock_face_bitmap);
  layer_add_child(window_layer, bitmap_layer_get_layer(clock_face_layer));

#ifdef MAKE_CHRONOGRAPH
  chrono_dial_tenths_bitmap_white = gbitmap_create_with_resource(RESOURCE_ID_CHRONO_DIAL_TENTHS_WHITE);
  chrono_dial_tenths_bitmap_black = gbitmap_create_with_resource(RESOURCE_ID_CHRONO_DIAL_TENTHS_BLACK);
  chrono_dial_hours_bitmap_white = gbitmap_create_with_resource(RESOURCE_ID_CHRONO_DIAL_HOURS_WHITE);
  chrono_dial_hours_bitmap_black = gbitmap_create_with_resource(RESOURCE_ID_CHRONO_DIAL_HOURS_BLACK);

  {
    int height = chrono_dial_tenths_bitmap_white->bounds.size.h;
    int width = chrono_dial_tenths_bitmap_white->bounds.size.w;
    int x = CHRONO_TENTH_HAND_X - width / 2;
    int y = CHRONO_TENTH_HAND_Y - height / 2;

    chrono_dial_layer = layer_create(GRect(x, y, width, height));
  }
  layer_set_update_proc(chrono_dial_layer, &chrono_dial_layer_update_callback);
  layer_add_child(window_layer, chrono_dial_layer);

  chrono_digital_window = window_create();

  struct WindowHandlers chrono_digital_window_handlers;
  memset(&chrono_digital_window_handlers, 0, sizeof(chrono_digital_window_handlers));
  chrono_digital_window_handlers.load = chrono_digital_window_load_handler;
  chrono_digital_window_handlers.appear = chrono_digital_window_appear_handler;
  chrono_digital_window_handlers.disappear = chrono_digital_window_disappear_handler;
  chrono_digital_window_handlers.unload = chrono_digital_window_unload_handler;
  window_set_window_handlers(chrono_digital_window, chrono_digital_window_handlers);

#endif  // MAKE_CHRONOGRAPH

  init_battery_gauge(window_layer, BATTERY_GAUGE_X, BATTERY_GAUGE_Y, BATTERY_GAUGE_ON_BLACK, false);
  init_bluetooth_indicator(window_layer, BLUETOOTH_X, BLUETOOTH_Y, BLUETOOTH_ON_BLACK, false);

#ifdef SHOW_DAY_CARD
  day_layer = layer_create(GRect(DAY_CARD_X - 15, DAY_CARD_Y - 8, 31, 19));
  layer_set_update_proc(day_layer, &day_layer_update_callback);
  layer_add_child(window_layer, day_layer);
#endif  // SHOW_DAY_CARD

#ifdef SHOW_DATE_CARD
  date_layer = layer_create(GRect(DATE_CARD_X - 15, DATE_CARD_Y - 8, 31, 19));
  layer_set_update_proc(date_layer, &date_layer_update_callback);
  layer_add_child(window_layer, date_layer);
#endif  // SHOW_DATE_CARD

  // Init all of the hands, taking care to arrange them in the correct
  // stacking order.
  for (i = 0; stacking_order[i] != STACKING_ORDER_DONE; ++i) {
    switch (stacking_order[i]) {
    case STACKING_ORDER_HOUR:
      hour_layer = layer_create(window_frame);
      layer_set_update_proc(hour_layer, &hour_layer_update_callback);
      layer_add_child(window_layer, hour_layer);
      break;

    case STACKING_ORDER_MINUTE:
      minute_layer = layer_create(window_frame);
      layer_set_update_proc(minute_layer, &minute_layer_update_callback);
      layer_add_child(window_layer, minute_layer);
      break;

    case STACKING_ORDER_SECOND:
      second_layer = layer_create(window_frame);
      layer_set_update_proc(second_layer, &second_layer_update_callback);
      layer_add_child(window_layer, second_layer);
      break;

    case STACKING_ORDER_CHRONO_MINUTE:
#ifdef SHOW_CHRONO_MINUTE_HAND
      chrono_minute_layer = layer_create(window_frame);
      layer_set_update_proc(chrono_minute_layer, &chrono_minute_layer_update_callback);
      layer_add_child(window_layer, chrono_minute_layer);
#endif  // SHOW_CHRONO_MINUTE_HAND
      break;

    case STACKING_ORDER_CHRONO_SECOND:
#ifdef SHOW_CHRONO_SECOND_HAND
      chrono_second_layer = layer_create(window_frame);
      layer_set_update_proc(chrono_second_layer, &chrono_second_layer_update_callback);
      layer_add_child(window_layer, chrono_second_layer);
#endif  // SHOW_CHRONO_SECOND_HAND
      break;

    case STACKING_ORDER_CHRONO_TENTH:
#ifdef SHOW_CHRONO_TENTH_HAND
      chrono_tenth_layer = layer_create(window_frame);
      layer_set_update_proc(chrono_tenth_layer, &chrono_tenth_layer_update_callback);
      layer_add_child(window_layer, chrono_tenth_layer);
#endif  // SHOW_CHRONO_TENTH_HAND
      break;
    }
  }

  update_chrono_laps_time();
  apply_config();
}


void handle_deinit() {
  save_chrono_data();
  tick_timer_service_unsubscribe();

  window_stack_pop_all(false);
  bitmap_layer_destroy(clock_face_layer);
  gbitmap_destroy(clock_face_bitmap);

#ifdef MAKE_CHRONOGRAPH
  layer_destroy(chrono_dial_layer);
  gbitmap_destroy(chrono_dial_tenths_bitmap_white);
  gbitmap_destroy(chrono_dial_tenths_bitmap_black);
  gbitmap_destroy(chrono_dial_hours_bitmap_white);
  gbitmap_destroy(chrono_dial_hours_bitmap_black);

  window_destroy(chrono_digital_window);
#endif  // MAKE_CHRONOGRAPH

  deinit_battery_gauge();
  deinit_bluetooth_indicator();

#ifdef SHOW_DAY_CARD
  layer_destroy(day_layer);
#endif
#ifdef SHOW_DATE_CARD
  layer_destroy(date_layer);
#endif
  layer_destroy(minute_layer);
  layer_destroy(hour_layer);
  layer_destroy(second_layer);
#ifdef SHOW_CHRONO_MINUTE_HAND
  layer_destroy(chrono_minute_layer);
#endif
#ifdef SHOW_CHRONO_SECOND_HAND
  layer_destroy(chrono_second_layer);
#endif
#ifdef SHOW_CHRONO_TENTH_HAND
  layer_destroy(chrono_tenth_layer);
#endif

  window_destroy(window);
}

int main(void) {
  handle_init();
  app_event_loop();
  handle_deinit();
}
