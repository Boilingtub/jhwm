#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/xwayland.h>

enum jewl_cursor_mode
 {
   JEWL_CURSOR_PASSTHROUGH,
   JEWL_CURSOR_MOVE,
   JEWL_CURSOR_RESIZE,
 };

struct jewl_server
 {
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer; 
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
   
    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_surface;
    struct wl_list views;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
   
    struct wlr_seat *seat;
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;
    struct wl_list keyboards;
    enum jewl_cursor_mode cursor_mode;
    struct jewl_view *grabbed_view;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;
 };

struct jewl_output
 {
   struct wl_list link;
   struct jewl_server *server;
   struct wlr_output *wlr_output;
   struct wl_listener frame;
   struct wl_listener destroy;
 };

struct jewl_view
 {
   struct wl_list link;
   struct jewl_server *server;
   struct wlr_xdg_toplevel *xdg_toplevel;
   struct wlr_scene_tree *scene_tree;
   struct wl_listener map;
   struct wl_listener unmap;
   struct wl_listener destroy;
   struct wl_listener request_move;
   struct wl_listener request_resize;
   struct wl_listener request_maximize;
   struct wl_listener request_fullscreen;
   int x ,y;
 };

struct jewl_keyboard
 {
  struct wl_list link;
  struct jewl_server *server;
  struct wlr_keyboard *wlr_keyboard;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
 };

static void focus_view(struct jewl_view *view, struct wlr_surface *surface)
 {
   if(view == NULL)
     {
        return;
     }
   struct jewl_server *server = view->server;
   struct wlr_seat *seat = server->seat;
   struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
   if(prev_surface == surface)
     {
       return; /*Don't re-focus an already focused surface*/
     }
   if(prev_surface)
     {
        struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(seat->keyboard_state.focused_surface);
        assert(previous->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);
        wlr_xdg_toplevel_set_activated(previous->toplevel , false);/*Deactivate prev surface*/  
     }
   struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
   /*Move view to front*/
   wlr_scene_node_raise_to_top(&view->scene_tree->node);
   wl_list_remove(&view->link);
   wl_list_insert(&server->views, &view->link);
   wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true); /*Activate new surface*/
   if(keyboard != NULL)
     {
        wlr_seat_keyboard_notify_enter(seat , view->xdg_toplevel->base->surface,
                                       keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
     }

 }

static void keyboard_handle_modifiers(struct wl_listener *listener , void *data)
 { 
   /*Struct to handel if modifier key is pressed*/
   struct jewl_keyboard *keyboard = wl_container_of(listener , keyboard ,modifiers);
   /*assign all keyboards to same seat , POSSIBLE: swap out wlr_keyboard and wlr_seat will handel*/
   wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
   wlr_seat_keyboard_notify_modifiers(keyboard->server->seat , &keyboard->wlr_keyboard->modifiers);
 }

static bool handel_keybindings(struct jewl_server *server , xkb_keysym_t sym)
 {
   switch (sym)
   {
    case XKB_KEY_Escape: wl_display_terminate(server->wl_display);
    break;

    case XKB_KEY_F1:     if(wl_list_length(&server->views) < 2) 
                           { break; }
                         struct jewl_view *next_view = wl_container_of(server->views.prev,
                                                                       next_view, link);
                         focus_view(next_view , next_view->xdg_toplevel->base->surface);
    break;

    default:             return false;
   }
  return true;
 }

static void keyboard_handle_key(struct wl_listener *listener , void *data)
 {
  /*when key pressed or released*/
  struct jewl_keyboard *keyboard = wl_container_of(listener, keyboard, key);
  struct jewl_server *server = keyboard->server;
  struct wlr_keyboard_key_event *event = data;
  struct wlr_seat *seat = server->seat;

  uint32_t keycode = event->keycode + 8; /*Convert libinput keycode -> xkbcommon*/
  const xkb_keysym_t *syms;/*get list of keysyms based on keymap of keyboard*/
  int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode , &syms);
  
  bool handled = false;
  uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
  if((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
    {
        /* if ALT is held down and other button pressed , call compositor keybindings */
        for(int i = 0; i < nsyms; i++)
           {
                handled = handel_keybindings(server , syms[i]);
           }
    }
  if(!handled)
    {
        wlr_seat_set_keyboard(seat , keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
 }

static void keyboard_handle_destroy(struct wl_listener *listener , void *data)
 {/*run if base wlr_input_device singals to destroy wlr_keyboard , no longer receive events*/
  struct jewl_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
  wl_list_remove(&keyboard->modifiers.link);
  wl_list_remove(&keyboard->key.link);
  wl_list_remove(&keyboard->destroy.link);
  wl_list_remove(&keyboard->link);
  free(keyboard);
 }

static void server_new_keyboard(struct jewl_server *server, struct wlr_input_device *device)
 {
  struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
  struct jewl_keyboard *keyboard = calloc(1 , sizeof(struct jewl_keyboard));
  keyboard->server = server;
  keyboard->wlr_keyboard = wlr_keyboard;
  /*prepare an XKB keymap and assig it to the keyboard, assume default layout (layout=us)*/
  struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_keymap *keymap = xkb_keymap_new_from_names(context , NULL , XKB_KEYMAP_COMPILE_NO_FLAGS);
  
  wlr_keyboard_set_keymap(wlr_keyboard, keymap);
  xkb_keymap_unref(keymap);
  xkb_context_unref(context);
  wlr_keyboard_set_repeat_info(wlr_keyboard , 25 , 600);
  /*listen fort keyboard events*/
  keyboard->modifiers.notify = keyboard_handle_modifiers;
  wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);

  keyboard->key.notify = keyboard_handle_key;
  wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);

  keyboard->destroy.notify = keyboard_handle_destroy;
  wl_signal_add(&device->events.destroy, &keyboard->destroy);

  wlr_seat_set_keyboard(server->seat , keyboard->wlr_keyboard);

  wl_list_insert(&server->keyboards , &keyboard->link);/*Add keyboard to list of keyboards*/
 }

static void server_new_pointer(struct jewl_server *server , struct wlr_input_device *device)
 {
  wlr_cursor_attach_input_device(server->cursor, device);
 }

static void server_new_input(struct wl_listener *listener , void *data)
 {
  struct jewl_server *server = wl_container_of(listener , server , new_input);
  struct wlr_input_device *device = data;
  
  switch(device->type)
    {
     case WLR_INPUT_DEVICE_KEYBOARD: server_new_keyboard(server , device);
     break;

     case WLR_INPUT_DEVICE_POINTER: server_new_pointer(server , device);
     break;

     default:
     break;
    }
  /*wlr_seat needs to know user capabilities [ pointer is always active ]*/
  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  if(!wl_list_empty(&server->keyboards))
    {
      caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
  wlr_seat_set_capabilities(server->seat, caps);      
   
 }

static void seat_request_cursor(struct wl_listener *listener , void *data)
 {
  struct jewl_server *server = wl_container_of(listener , server , request_cursor);
   /*run by seat if client provides cursor image*/
  struct wlr_seat_pointer_request_set_cursor_event *event = data;
  struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
   /*sent by any client check to see if this one has pointer focus*/
  if(focused_client == event->seat_client)
    {
     /*tell cursor to use current surface as cursor image set hardware cursor to and
      * continue as cursor moves between outputs*/
     wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x , event->hotspot_y);
    }
 }

static void seat_request_set_selection(struct wl_listener *listener , void *data)
 {/*run when client want to set a selection , when user copies something*/
   struct jewl_server *server = wl_container_of(listener , server ,request_set_selection);
   struct wlr_seat_request_set_selection_event *event = data;
   wlr_seat_set_selection(server->seat , event->source, event->serial);
 }

static struct jewl_view *desktop_view_at(struct jewl_server *server , double lx , double ly,
                                         struct wlr_surface **surface, double *sx, double *sy)
 {
   struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node , lx, ly, sx, sy);
   if(node == NULL || node->type != WLR_SCENE_NODE_BUFFER)
     {
       return NULL;
     }
   struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
   struct wlr_scene_surface *scene_surface = wlr_scene_surface_from_buffer(scene_buffer);
   if(!scene_surface)
     {
      return NULL;
     }
 
   *surface = scene_surface->surface;
   /*find node corresponding to jewl_view at root of surface tree*/
   struct wlr_scene_tree *tree = node->parent;
   while(tree != NULL && tree->node.data == NULL)
        {
          tree = tree->node.parent;
        }
   return tree->node.data;
 }

static void process_cursor_move(struct jewl_server *server, uint32_t time)
 {
  struct jewl_view *view = server->grabbed_view;
  view->x = server->cursor->x - server->grab_x;
  view->y = server->cursor->y - server->grab_y;
  wlr_scene_node_set_position(&view->scene_tree->node, view->x , view->y);
 }

static void process_cursor_resize(struct jewl_server *server, uint32_t time)
 {
  struct jewl_view *view = server->grabbed_view;
  double border_x = server->cursor->x - server->grab_x;
  double border_y = server->cursor->y - server->grab_y;

  int new_left = server->grab_geobox.x;
  int new_right = server->grab_geobox.x + server->grab_geobox.width;
  
  int new_top = server->grab_geobox.y;
  int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

  if(server->resize_edges & WLR_EDGE_TOP)
    {
        new_top = border_y;
        if(new_top >= new_bottom)
          {
           new_top = new_bottom - 1;
          }
    }
  else 
  if(server->resize_edges & WLR_EDGE_BOTTOM)
    {
     new_bottom = border_y;
     if(new_bottom <= new_top)
       {
         new_bottom = new_top + 1;
       }
    }
  
  if(server->resize_edges & WLR_EDGE_LEFT)
    {
     new_left = border_x;
     if(new_left >= new_right)
       {
         new_left = new_right - 1;   
       }
    }
  else
  if(server->resize_edges & WLR_EDGE_RIGHT)
    {
      new_right = border_x;
      if(new_right <= new_left)
        {
          new_right = new_left + 1;
        }
    }
   
  struct wlr_box geo_box;
  wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo_box);
  view->x = new_left - geo_box.x;
  view->y = new_top - geo_box.y;
  wlr_scene_node_set_position(&view->scene_tree->node, view->x , view->y);

  int new_width = new_right - new_left;
  int new_height = new_bottom - new_top;      
  wlr_xdg_toplevel_set_size(view->xdg_toplevel, new_width , new_height);
 }

static void process_cursor_motion(struct jewl_server *server, uint32_t time)
 {/*if the mode is non-passthrough, dekegate to those functions*/
   if(server->cursor_mode == JEWL_CURSOR_MOVE)
     {
        process_cursor_move(server, time);
        return;
     }else
    if(server->cursor_mode == JEWL_CURSOR_RESIZE)
      {
        process_cursor_resize(server , time);
        return;
      }
   /*otherwise, find the view under the pointer and sent the event along*/
   double sx , sy; 
   struct wlr_seat *seat = server->seat;
   struct wlr_surface *surface = NULL;
   struct jewl_view *view = desktop_view_at(server, server->cursor->x,
                                              server->cursor->y, &surface , &sx , &sy);
   if(!view)
     {/*if no view under cursor, set cursor image to default. makes cursor image appear*/
        wlr_xcursor_manager_set_cursor_image(server->cursor_mgr, "left_ptr", server->cursor);
     }
   if(surface)
     {
        /* send pointer enter and motion events
         * enter event gives surface "pointer focus", which is different from keyboard focus
         * get pointer focus by moving the pointer over a window*/
        wlr_seat_pointer_notify_enter(seat , surface ,sx , sy);
        wlr_seat_pointer_notify_motion(seat , time ,sx ,sy);
     } else
        {/*Clear pointer focus so future events not sent to last client with cursor*/
          wlr_seat_pointer_clear_focus(seat); 
        }
 }

static void server_cursor_motion(struct wl_listener *listener, void *data)
 {/*run when cursor emits a _relative_ pointer motion event (i.e a delta)*/ 
  struct jewl_server *server = wl_container_of(listener, server, cursor_motion);
  struct wlr_pointer_motion_event *event = data;
  /*cursor does not move unless told to ,  can pass NULL to move without input */
  wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
  process_cursor_motion(server, event->time_msec);
 }

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data)
 {/*run when cursor emits a _absolute_ motion event, 1..0 on each axis*/
  struct jewl_server *server = wl_container_of(listener, server, cursor_motion_absolute);
  struct wlr_pointer_motion_absolute_event *event = data;
  wlr_cursor_wrap_absolute(server->cursor, &event->pointer->base, event->x, event->y);
  process_cursor_motion(server, event->time_msec);
 }

static void server_cursor_button(struct wl_listener *listener, void *data)
 {/*run when cursor pointer emits a button event*/
  struct jewl_server *server = wl_container_of(listener, server, cursor_button);
  struct wlr_pointer_button_event *event = data;
  /*Notify client with pointer focus that button press has occured*/
  wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
  double sx, sy;
  struct wlr_surface *surface = NULL;
  struct jewl_view *view = desktop_view_at(server , server->cursor->x, server->cursor->y,
                                           &surface, &sx, &sy);
  if(event->state == WLR_BUTTON_RELEASED)
    {
        /*if released any buttons , exit interactive move/resize mode*/
        server->cursor_mode = JEWL_CURSOR_PASSTHROUGH;
    }
    else
    {/*focus that client if button was _pressed_*/
        focus_view(view, surface);
    }
 }

static void server_cursor_axis(struct wl_listener *listener, void *data)
 {/*run when cursor pointer emits axis event e.g scroll wheel move*/
   struct jewl_server *server = wl_container_of(listener, server, cursor_axis);
   struct wlr_pointer_axis_event *event = data;
   /*Notify the clinet with pointer focus of the axis event*/
   wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
                                event->delta, event->delta_discrete, event->source);
 }

static void server_cursor_frame(struct wl_listener *listener, void *data)
 {

 }//537

int main()
 {

 }












































 






