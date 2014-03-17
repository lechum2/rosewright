#include <pebble.h>
#include "battery_gauge.h"
#include "config_options.h"
#include "bwd.h"

BitmapWithData battery_gauge_empty;
BitmapWithData battery_gauge_charging;
Layer *battery_gauge_layer;

bool battery_gauge_on_black = false;
bool battery_gauge_opaque_layer = false;

void battery_gauge_layer_update_callback(Layer *me, GContext *ctx) {
  BatteryChargeState charge_state = battery_state_service_peek();

#ifdef BATTERY_HACK
  time_t now = time(NULL);  
  charge_state.charge_percent = 100 - ((now / 2) % 11) * 10;
#endif  // BATTERY_HACK

  if (battery_gauge_opaque_layer) {
    if (charge_state.is_charging || config.keep_battery_gauge || (charge_state.is_plugged || charge_state.charge_percent <= 20)) {
      // Draw the background of the layer.
      GRect box = layer_get_frame(me);
      box.origin.x = 0;
      box.origin.y = 0;
      if (battery_gauge_on_black ^ config.draw_mode) {
	graphics_context_set_fill_color(ctx, GColorBlack);
      } else {
	graphics_context_set_fill_color(ctx, GColorWhite);
      }
      graphics_fill_rect(ctx, box, 0, GCornerNone);
    }
  }

  GRect box = layer_get_frame(me);
  box.origin.x = 1;
  box.origin.y = 0;
  box.size.w -= 2;

  if (battery_gauge_on_black ^ config.draw_mode) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_context_set_fill_color(ctx, GColorWhite);
  } else {
    graphics_context_set_compositing_mode(ctx, GCompOpAnd);
    graphics_context_set_fill_color(ctx, GColorBlack);
  }

  if (charge_state.is_charging) {
    graphics_draw_bitmap_in_rect(ctx, battery_gauge_charging.bitmap, box);
  } else if (config.keep_battery_gauge || (charge_state.is_plugged || charge_state.charge_percent <= 20)) {
    // Unless keep_battery_gauge is configured true, then we don't
    // bother showing the battery gauge when it's in a normal
    // condition.
    graphics_draw_bitmap_in_rect(ctx, battery_gauge_empty.bitmap, box);
    int bar_width = (charge_state.charge_percent * 9 + 50) / 100 + 1;
    graphics_fill_rect(ctx, GRect(3, 3, bar_width, 4), 0, GCornerNone);
  }
}

// Update the battery guage.
void handle_battery(BatteryChargeState charge_state) {
  layer_mark_dirty(battery_gauge_layer);
}

void init_battery_gauge(Layer *window_layer, int x, int y, bool on_black, bool opaque_layer) {
  battery_gauge_on_black = on_black;
  battery_gauge_opaque_layer = opaque_layer;
  battery_gauge_empty = png_bwd_create(RESOURCE_ID_BATTERY_GAUGE_EMPTY);
  battery_gauge_charging = png_bwd_create(RESOURCE_ID_BATTERY_GAUGE_CHARGING);
  battery_gauge_layer = layer_create(GRect(x, y, 18, 10));
  layer_set_update_proc(battery_gauge_layer, &battery_gauge_layer_update_callback);
  layer_add_child(window_layer, battery_gauge_layer);
  battery_state_service_subscribe(&handle_battery);
}

void deinit_battery_gauge() {
  battery_state_service_unsubscribe();
  layer_destroy(battery_gauge_layer);
  bwd_destroy(&battery_gauge_empty);
  bwd_destroy(&battery_gauge_charging);
}

void refresh_battery_gauge() {
  layer_mark_dirty(battery_gauge_layer);
}
