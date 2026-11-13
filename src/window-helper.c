/* window-helper.c
 * Implements the helper that tries to keep GtkWindow above other windows
 *
 * Copyright 2022 abb128
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "window-helper.h"

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>

static bool set_window_keep_above_wayland(GtkWindow *window, bool keep_above) {
    // TODO: Plasma: https://wayland.app/protocols/kde-plasma-window-management#org_kde_plasma_window_management:enum:state:entry:keep_above

    return false;
}
#endif

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>

#define _NET_WM_STATE_REMOVE        0    /* remove/unset property */
#define _NET_WM_STATE_ADD           1    /* add/set property */
#define _NET_WM_STATE_TOGGLE        2    /* toggle property  */
static bool set_window_keep_above_x11(GtkWindow *window, bool keep_above) {
    GdkX11Display *display = GDK_X11_DISPLAY(gtk_widget_get_display(GTK_WIDGET(window)));
    GdkX11Surface *surface = GDK_X11_SURFACE(gtk_native_get_surface(gtk_widget_get_native(GTK_WIDGET(window))));
    Display *xdisplay = gdk_x11_display_get_xdisplay(display);

    Window xrootwindow = gdk_x11_display_get_xrootwindow(display);
    Window xwindow = gdk_x11_surface_get_xid(surface);

    Atom wm_state_atom = XInternAtom(xdisplay, "_NET_WM_STATE", false);
    Atom keep_above_atom = XInternAtom(xdisplay, "_NET_WM_STATE_ABOVE", false);

    XEvent e;
    memset(&e, 0, sizeof(e));
    e.xclient.type = ClientMessage;
    e.xclient.message_type = wm_state_atom;
    e.xclient.display = xdisplay;
    e.xclient.window = xwindow;
    e.xclient.format = 32;
    e.xclient.data.l[0] = keep_above ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
    e.xclient.data.l[1] = keep_above_atom;

    XSendEvent(xdisplay, xrootwindow, false, SubstructureRedirectMask | SubstructureNotifyMask, &e);

    return true;
}

static bool move_window_to_bottom_center_x11(GtkWindow *window, int bottom_margin) {
    GtkNative *native = gtk_widget_get_native(GTK_WIDGET(window));
    GdkSurface *surface = gtk_native_get_surface(native);
    if(surface == NULL) return false;

    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
    GdkMonitor *monitor = gdk_display_get_monitor_at_surface(display, surface);
    if(monitor == NULL) return false;

    // Work area excludes panels/docks; coordinates are in GDK (scaled) pixels
    GdkRectangle area;
    gdk_x11_monitor_get_workarea(monitor, &area);

    // The surface also holds the client-side shadow; place the visible window
    double shadow_x = 0, shadow_y = 0;
    gtk_native_get_surface_transform(native, &shadow_x, &shadow_y);
    int width = gtk_widget_get_width(GTK_WIDGET(window));
    int height = gtk_widget_get_height(GTK_WIDGET(window));
    if(width <= 0 || height <= 0) return false;

    int x = area.x + (area.width - width) / 2 - (int)shadow_x;
    int y = area.y + area.height - bottom_margin - height - (int)shadow_y;

    int scale = gdk_surface_get_scale_factor(surface);
    Display *xdisplay = gdk_x11_display_get_xdisplay(GDK_X11_DISPLAY(display));
    XMoveWindow(xdisplay, gdk_x11_surface_get_xid(GDK_X11_SURFACE(surface)), x * scale, y * scale);
    XFlush(xdisplay);
    return true;
}
#endif


static bool is_keep_above_overridden = false;

void override_keep_above_system(bool override) {
    is_keep_above_overridden = override;
}

bool is_keep_above_supported(GtkWindow *window) {
    if(is_keep_above_overridden) return true;

    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));

    #ifdef GDK_WINDOWING_X11
    if(GDK_IS_X11_DISPLAY(display))
      {
        return true;
      }
    else
    #endif
    #ifdef GDK_WINDOWING_WAYLAND
    if(GDK_IS_WAYLAND_DISPLAY(display))
      {
        return false; // TODO: Plasma
      }
    else
    #endif
    {
        return false;
    }
}

bool move_window_to_bottom_center(GtkWindow *window, int bottom_margin) {
#ifdef GDK_WINDOWING_X11
    if(GDK_IS_X11_DISPLAY(gtk_widget_get_display(GTK_WIDGET(window))))
        return move_window_to_bottom_center_x11(window, bottom_margin);
#endif
    (void)window;
    (void)bottom_margin;
    return false;
}

bool set_window_keep_above(GtkWindow *window, bool keep_above) {
    if(is_keep_above_overridden) return true;

    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));

    #ifdef GDK_WINDOWING_X11
    if(GDK_IS_X11_DISPLAY(display))
      {
        return set_window_keep_above_x11(window, keep_above);
      }
    else
    #endif
    #ifdef GDK_WINDOWING_WAYLAND
    if(GDK_IS_WAYLAND_DISPLAY(display))
      {
        return set_window_keep_above_wayland(window, keep_above);
      }
    else
    #endif
    {
        printf("set_window_keep_above: Unsupported compositor protocol\n");
    }

    return false;
}
