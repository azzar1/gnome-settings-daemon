/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 * Copyright (C) 2011-2012 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2011 Ritesh Khadgaray <khadgaray@gmail.com>
 * Copyright (C) 2012-2013 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <libupower-glib/upower.h>
#include <libnotify/notify.h>
#include <canberra-gtk.h>
#include <glib-unix.h>
#include <gio/gunixfdlist.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-rr.h>
#include <libgnome-desktop/gnome-idle-monitor.h>

#include <gsd-input-helper.h>

#include "gsd-power-constants.h"
#include "gsm-inhibitor-flag.h"
#include "gsm-presence-flag.h"
#include "gsm-manager-logout-mode.h"
#include "gpm-common.h"
#include "gnome-settings-plugin.h"
#include "gnome-settings-profile.h"
#include "gnome-settings-bus.h"
#include "gsd-enums.h"
#include "gsd-power-manager.h"

#define GNOME_SESSION_DBUS_NAME                 "org.gnome.SessionManager"
#define GNOME_SESSION_DBUS_PATH_PRESENCE        "/org/gnome/SessionManager/Presence"
#define GNOME_SESSION_DBUS_INTERFACE_PRESENCE   "org.gnome.SessionManager.Presence"

#define UPOWER_DBUS_NAME                        "org.freedesktop.UPower"
#define UPOWER_DBUS_PATH                        "/org/freedesktop/UPower"
#define UPOWER_DBUS_PATH_KBDBACKLIGHT           "/org/freedesktop/UPower/KbdBacklight"
#define UPOWER_DBUS_INTERFACE                   "org.freedesktop.UPower"
#define UPOWER_DBUS_INTERFACE_KBDBACKLIGHT      "org.freedesktop.UPower.KbdBacklight"

#define GSD_POWER_SETTINGS_SCHEMA               "org.gnome.settings-daemon.plugins.power"
#define GSD_XRANDR_SETTINGS_SCHEMA              "org.gnome.settings-daemon.plugins.xrandr"

#define GSD_POWER_DBUS_NAME                     GSD_DBUS_NAME ".Power"
#define GSD_POWER_DBUS_PATH                     GSD_DBUS_PATH "/Power"
#define GSD_POWER_DBUS_INTERFACE                GSD_DBUS_BASE_INTERFACE ".Power"
#define GSD_POWER_DBUS_INTERFACE_SCREEN         GSD_POWER_DBUS_INTERFACE ".Screen"
#define GSD_POWER_DBUS_INTERFACE_KEYBOARD       GSD_POWER_DBUS_INTERFACE ".Keyboard"

#define GSD_POWER_MANAGER_NOTIFY_TIMEOUT_SHORT          10 * 1000 /* ms */
#define GSD_POWER_MANAGER_NOTIFY_TIMEOUT_LONG           30 * 1000 /* ms */

#define SYSTEMD_DBUS_NAME                       "org.freedesktop.login1"
#define SYSTEMD_DBUS_PATH                       "/org/freedesktop/login1"
#define SYSTEMD_DBUS_INTERFACE                  "org.freedesktop.login1.Manager"

/* Time between notifying the user about a critical action and executing it.
 * This can be changed with the GSD_ACTION_DELAY constant. */
#ifndef GSD_ACTION_DELAY
#define GSD_ACTION_DELAY 20
#endif /* !GSD_ACTION_DELAY */

static const gchar introspection_xml[] =
"<node>"
"  <interface name='org.gnome.SettingsDaemon.Power.Screen'>"
"    <property name='Brightness' type='i' access='readwrite'/>"
"    <method name='StepUp'>"
"      <arg type='i' name='new_percentage' direction='out'/>"
"    </method>"
"    <method name='StepDown'>"
"      <arg type='i' name='new_percentage' direction='out'/>"
"    </method>"
"  </interface>"
"  <interface name='org.gnome.SettingsDaemon.Power.Keyboard'>"
"    <property name='Brightness' type='i' access='readwrite'/>"
"    <method name='StepUp'>"
"      <arg type='i' name='new_percentage' direction='out'/>"
"    </method>"
"    <method name='StepDown'>"
"      <arg type='i' name='new_percentage' direction='out'/>"
"    </method>"
"    <method name='Toggle'>"
"      <arg type='i' name='new_percentage' direction='out'/>"
"    </method>"
"  </interface>"
"</node>";

#define GSD_POWER_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_POWER_MANAGER, GsdPowerManagerPrivate))

typedef enum {
        GSD_POWER_IDLE_MODE_NORMAL,
        GSD_POWER_IDLE_MODE_DIM,
        GSD_POWER_IDLE_MODE_BLANK,
        GSD_POWER_IDLE_MODE_SLEEP
} GsdPowerIdleMode;

struct GsdPowerManagerPrivate
{
        /* D-Bus */
        GsdSessionManager       *session;
        guint                    name_id;
        GDBusNodeInfo           *introspection_data;
        GDBusConnection         *connection;
        GCancellable            *bus_cancellable;
        GDBusProxy              *session_presence_proxy;

        /* Settings */
        GSettings               *settings;
        GSettings               *settings_bus;
        GSettings               *settings_screensaver;
        GSettings               *settings_xrandr;

        /* Screensaver */
        GsdScreenSaver          *screensaver_proxy;
        gboolean                 screensaver_active;

        /* State */
        gboolean                 lid_is_present;
        gboolean                 lid_is_closed;
        UpClient                *up_client;
        GPtrArray               *devices_array;
        UpDevice                *device_composite;
        GnomeRRScreen           *rr_screen;
        NotifyNotification      *notification_ups_discharging;
        NotifyNotification      *notification_low;
        NotifyNotification      *notification_sleep_warning;
        NotifyNotification      *notification_logout_warning;
        GsdPowerActionType       sleep_action_type;
        gboolean                 battery_is_low; /* laptop battery low, or UPS discharging */

        /* Brightness */
        gboolean                 backlight_available;
        gint                     pre_dim_brightness; /* level, not percentage */

        /* Keyboard */
        GDBusProxy              *upower_kdb_proxy;
        gint                     kbd_brightness_now;
        gint                     kbd_brightness_max;
        gint                     kbd_brightness_old;
        gint                     kbd_brightness_pre_dim;

        /* Sound */
        guint32                  critical_alert_timeout_id;

        /* systemd stuff */
        GDBusProxy              *logind_proxy;
        gint                     inhibit_lid_switch_fd;
        gboolean                 inhibit_lid_switch_taken;
        gint                     inhibit_suspend_fd;
        gboolean                 inhibit_suspend_taken;
        guint                    inhibit_lid_switch_timer_id;
        gboolean                 is_virtual_machine;

        /* Idles */
        GnomeIdleMonitor        *idle_monitor;
        guint                    idle_dim_id;
        guint                    idle_blank_id;
        guint                    idle_sleep_warning_id;
        guint                    idle_sleep_id;
        GsdPowerIdleMode         current_idle_mode;

        guint                    temporary_unidle_on_ac_id;
        GsdPowerIdleMode         previous_idle_mode;

        guint                    xscreensaver_watchdog_timer_id;
};

enum {
        PROP_0,
};

static void     gsd_power_manager_class_init  (GsdPowerManagerClass *klass);
static void     gsd_power_manager_init        (GsdPowerManager      *power_manager);

static void      engine_device_warning_changed_cb (UpDevice *device, GParamSpec *pspec, GsdPowerManager *manager);
static void      do_power_action_type (GsdPowerManager *manager, GsdPowerActionType action_type);
static void      uninhibit_lid_switch (GsdPowerManager *manager);
static void      main_battery_or_ups_low_changed (GsdPowerManager *manager, gboolean is_low);
static gboolean  idle_is_session_inhibited (GsdPowerManager *manager, guint mask, gboolean *is_inhibited);
static void      idle_triggered_idle_cb (GnomeIdleMonitor *monitor, guint watch_id, gpointer user_data);
static void      idle_became_active_cb (GnomeIdleMonitor *monitor, guint watch_id, gpointer user_data);

G_DEFINE_TYPE (GsdPowerManager, gsd_power_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

GQuark
gsd_power_manager_error_quark (void)
{
        static GQuark quark = 0;
        if (!quark)
                quark = g_quark_from_static_string ("gsd_power_manager_error");
        return quark;
}

static void
notify_close_if_showing (NotifyNotification **notification)
{
        if (*notification == NULL)
                return;
        notify_notification_close (*notification, NULL);
        g_clear_object (notification);
}

static void
engine_device_add (GsdPowerManager *manager, UpDevice *device)
{
        UpDeviceKind kind;

        /* Batteries and UPSes are already handled through
         * the composite battery */
        g_object_get (device, "kind", &kind, NULL);
        if (kind == UP_DEVICE_KIND_BATTERY ||
            kind == UP_DEVICE_KIND_UPS ||
            kind == UP_DEVICE_KIND_LINE_POWER)
                return;
        g_ptr_array_add (manager->priv->devices_array, g_object_ref (device));

        g_signal_connect (device, "notify::warning-level",
                          G_CALLBACK (engine_device_warning_changed_cb), manager);

        engine_device_warning_changed_cb (device, NULL, manager);
}

static gboolean
engine_coldplug (GsdPowerManager *manager)
{
        guint i;
        GPtrArray *array = NULL;
        UpDevice *device;
        gboolean ret;
        GError *error = NULL;

        /* get devices from UPower */
        ret = up_client_enumerate_devices_sync (manager->priv->up_client, NULL, &error);
        if (!ret) {
                g_warning ("failed to get device list: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        /* add to database */
        array = up_client_get_devices (manager->priv->up_client);

        for (i = 0 ; array != NULL && i < array->len ; i++) {
                device = g_ptr_array_index (array, i);
                engine_device_add (manager, device);
        }

        g_clear_pointer (&array, g_ptr_array_unref);

        /* never repeat */
        return FALSE;
}

static void
engine_device_added_cb (UpClient *client, UpDevice *device, GsdPowerManager *manager)
{
        engine_device_add (manager, device);
}

static void
engine_device_removed_cb (UpClient *client, UpDevice *device, GsdPowerManager *manager)
{
        g_ptr_array_remove (manager->priv->devices_array, device);
}

static void
on_notification_closed (NotifyNotification *notification, gpointer data)
{
    g_object_unref (notification);
}

static void
create_notification (const char *summary,
                     const char *body,
                     const char *icon_name,
                     NotifyNotification **weak_pointer_location)
{
        NotifyNotification *notification;

        notification = notify_notification_new (summary, body, icon_name);
        *weak_pointer_location = notification;
        g_object_add_weak_pointer (G_OBJECT (notification),
                                   (gpointer *) weak_pointer_location);
        g_signal_connect (notification, "closed",
                          G_CALLBACK (on_notification_closed), NULL);
}

static void
engine_ups_discharging (GsdPowerManager *manager, UpDevice *device)
{
        const gchar *title;
        gchar *remaining_text = NULL;
        gdouble percentage;
        char *icon_name;
        gint64 time_to_empty;
        GString *message;

        /* get device properties */
        g_object_get (device,
                      "percentage", &percentage,
                      "time-to-empty", &time_to_empty,
                      "icon-name", &icon_name,
                      NULL);

        main_battery_or_ups_low_changed (manager, TRUE);

        /* only show text if there is a valid time */
        if (time_to_empty > 0)
                remaining_text = gpm_get_timestring (time_to_empty);

        /* TRANSLATORS: UPS is now discharging */
        title = _("UPS Discharging");

        message = g_string_new ("");
        if (remaining_text != NULL) {
                /* TRANSLATORS: tell the user how much time they have got */
                g_string_append_printf (message, _("%s of UPS backup power remaining"),
                                        remaining_text);
        } else {
                g_string_append (message, _("Unknown amount of UPS backup power remaining"));
        }
        g_string_append_printf (message, " (%.0f%%)", percentage);

        /* close any existing notification of this class */
        notify_close_if_showing (&manager->priv->notification_ups_discharging);

        /* create a new notification */
        create_notification (title, message->str,
                             icon_name,
                             &manager->priv->notification_ups_discharging);
        notify_notification_set_timeout (manager->priv->notification_ups_discharging,
                                         GSD_POWER_MANAGER_NOTIFY_TIMEOUT_LONG);
        notify_notification_set_urgency (manager->priv->notification_ups_discharging,
                                         NOTIFY_URGENCY_NORMAL);
        /* TRANSLATORS: this is the notification application name */
        notify_notification_set_app_name (manager->priv->notification_ups_discharging, _("Power"));
        notify_notification_set_hint (manager->priv->notification_ups_discharging,
                                      "transient", g_variant_new_boolean (TRUE));

        notify_notification_show (manager->priv->notification_ups_discharging, NULL);

        g_string_free (message, TRUE);
        g_free (icon_name);
        g_free (remaining_text);
}

static GsdPowerActionType
manager_critical_action_get (GsdPowerManager *manager)
{
        GsdPowerActionType policy;
        char *action;

        action = up_client_get_critical_action (manager->priv->up_client);
        /* We don't make the difference between HybridSleep and Hibernate */
        if (g_strcmp0 (action, "PowerOff") == 0)
                policy = GSD_POWER_ACTION_SHUTDOWN;
        else
                policy = GSD_POWER_ACTION_HIBERNATE;
        g_free (action);
        return policy;
}

static gboolean
manager_critical_action_do_cb (GsdPowerManager *manager)
{
        /* stop playing the alert as it's too late to do anything now */
        play_loop_stop (&manager->priv->critical_alert_timeout_id);

        return FALSE;
}

static void
engine_charge_low (GsdPowerManager *manager, UpDevice *device)
{
        const gchar *title = NULL;
        gboolean ret;
        gchar *message = NULL;
        gchar *tmp;
        gchar *remaining_text;
        gdouble percentage;
        char *icon_name;
        gint64 time_to_empty;
        UpDeviceKind kind;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      "percentage", &percentage,
                      "time-to-empty", &time_to_empty,
                      "icon-name", &icon_name,
                      NULL);

        /* check to see if the batteries have not noticed we are on AC */
        if (kind == UP_DEVICE_KIND_BATTERY) {
                if (!up_client_get_on_battery (manager->priv->up_client)) {
                        g_warning ("ignoring critically low message as we are not on battery power");
                        goto out;
                }
        }

        if (kind == UP_DEVICE_KIND_BATTERY) {

                /* if the user has no other batteries, drop the "Laptop" wording */
                ret = (manager->priv->devices_array->len > 0);
                if (ret) {
                        /* TRANSLATORS: laptop battery low, and we only have one battery */
                        title = _("Battery low");
                } else {
                        /* TRANSLATORS: laptop battery low, and we have more than one kind of battery */
                        title = _("Laptop battery low");
                }
                tmp = gpm_get_timestring (time_to_empty);
                remaining_text = g_strconcat ("<b>", tmp, "</b>", NULL);
                g_free (tmp);

                /* TRANSLATORS: tell the user how much time they have got */
                message = g_strdup_printf (_("Approximately %s remaining (%.0f%%)"), remaining_text, percentage);
                g_free (remaining_text);

                main_battery_or_ups_low_changed (manager, TRUE);

        } else if (kind == UP_DEVICE_KIND_UPS) {
                /* TRANSLATORS: UPS is starting to get a little low */
                title = _("UPS low");
                tmp = gpm_get_timestring (time_to_empty);
                remaining_text = g_strconcat ("<b>", tmp, "</b>", NULL);
                g_free (tmp);

                /* TRANSLATORS: tell the user how much time they have got */
                message = g_strdup_printf (_("Approximately %s of remaining UPS backup power (%.0f%%)"),
                                           remaining_text, percentage);
                g_free (remaining_text);
        } else if (kind == UP_DEVICE_KIND_MOUSE) {
                /* TRANSLATORS: mouse is getting a little low */
                title = _("Mouse battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Wireless mouse is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_KEYBOARD) {
                /* TRANSLATORS: keyboard is getting a little low */
                title = _("Keyboard battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Wireless keyboard is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_PDA) {
                /* TRANSLATORS: PDA is getting a little low */
                title = _("PDA battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("PDA is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_PHONE) {
                /* TRANSLATORS: cell phone (mobile) is getting a little low */
                title = _("Cell phone battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Cell phone is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_MEDIA_PLAYER) {
                /* TRANSLATORS: media player, e.g. mp3 is getting a little low */
                title = _("Media player battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Media player is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_TABLET) {
                /* TRANSLATORS: graphics tablet, e.g. wacom is getting a little low */
                title = _("Tablet battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Tablet is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_COMPUTER) {
                /* TRANSLATORS: computer, e.g. ipad is getting a little low */
                title = _("Attached computer battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Attached computer is low in power (%.0f%%)"), percentage);
        }

        /* close any existing notification of this class */
        notify_close_if_showing (&manager->priv->notification_low);

        /* create a new notification */
        create_notification (title, message,
                             icon_name,
                             &manager->priv->notification_low);
        notify_notification_set_timeout (manager->priv->notification_low,
                                         GSD_POWER_MANAGER_NOTIFY_TIMEOUT_LONG);
        notify_notification_set_urgency (manager->priv->notification_low,
                                         NOTIFY_URGENCY_NORMAL);
        notify_notification_set_app_name (manager->priv->notification_low, _("Power"));
        notify_notification_set_hint (manager->priv->notification_low,
                                      "transient", g_variant_new_boolean (TRUE));

        notify_notification_show (manager->priv->notification_low, NULL);

        /* play the sound, using sounds from the naming spec */
        ca_context_play (ca_gtk_context_get (), 0,
                         CA_PROP_EVENT_ID, "battery-low",
                         /* TRANSLATORS: this is the sound description */
                         CA_PROP_EVENT_DESCRIPTION, _("Battery is low"), NULL);

out:
        g_free (icon_name);
        g_free (message);
}

static void
engine_charge_critical (GsdPowerManager *manager, UpDevice *device)
{
        const gchar *title = NULL;
        gboolean ret;
        gchar *message = NULL;
        gdouble percentage;
        char *icon_name;
        gint64 time_to_empty;
        GsdPowerActionType policy;
        UpDeviceKind kind;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      "percentage", &percentage,
                      "time-to-empty", &time_to_empty,
                      "icon-name", &icon_name,
                      NULL);

        /* check to see if the batteries have not noticed we are on AC */
        if (kind == UP_DEVICE_KIND_BATTERY) {
                if (!up_client_get_on_battery (manager->priv->up_client)) {
                        g_warning ("ignoring low message as we are not on battery power");
                        goto out;
                }
        }

        if (kind == UP_DEVICE_KIND_BATTERY) {

                /* if the user has no other batteries, drop the "Laptop" wording */
                ret = (manager->priv->devices_array->len > 0);
                if (ret) {
                        /* TRANSLATORS: laptop battery critically low, and only have one kind of battery */
                        title = _("Battery critically low");
                } else {
                        /* TRANSLATORS: laptop battery critically low, and we have more than one type of battery */
                        title = _("Laptop battery critically low");
                }

                /* we have to do different warnings depending on the policy */
                policy = manager_critical_action_get (manager);

                /* use different text for different actions */
                if (policy == GSD_POWER_ACTION_HIBERNATE) {
                        /* TRANSLATORS: give the user a ultimatum */
                        message = g_strdup_printf (_("Computer will hibernate very soon unless it is plugged in."));

                } else if (policy == GSD_POWER_ACTION_SHUTDOWN) {
                        /* TRANSLATORS: give the user a ultimatum */
                        message = g_strdup_printf (_("Computer will shutdown very soon unless it is plugged in."));
                }

                main_battery_or_ups_low_changed (manager, TRUE);

        } else if (kind == UP_DEVICE_KIND_UPS) {
                gchar *remaining_text;
                gchar *tmp;

                /* TRANSLATORS: the UPS is very low */
                title = _("UPS critically low");
                tmp = gpm_get_timestring (time_to_empty);
                remaining_text = g_strconcat ("<b>", tmp, "</b>", NULL);
                g_free (tmp);

                /* TRANSLATORS: give the user a ultimatum */
                message = g_strdup_printf (_("Approximately %s of remaining UPS power (%.0f%%). "
                                             "Restore AC power to your computer to avoid losing data."),
                                           remaining_text, percentage);
                g_free (remaining_text);
        } else if (kind == UP_DEVICE_KIND_MOUSE) {
                /* TRANSLATORS: the mouse battery is very low */
                title = _("Mouse battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Wireless mouse is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);
        } else if (kind == UP_DEVICE_KIND_KEYBOARD) {
                /* TRANSLATORS: the keyboard battery is very low */
                title = _("Keyboard battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Wireless keyboard is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);
        } else if (kind == UP_DEVICE_KIND_PDA) {

                /* TRANSLATORS: the PDA battery is very low */
                title = _("PDA battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("PDA is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);

        } else if (kind == UP_DEVICE_KIND_PHONE) {

                /* TRANSLATORS: the cell battery is very low */
                title = _("Cell phone battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Cell phone is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);
        } else if (kind == UP_DEVICE_KIND_MEDIA_PLAYER) {

                /* TRANSLATORS: the cell battery is very low */
                title = _("Cell phone battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Media player is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);
        } else if (kind == UP_DEVICE_KIND_TABLET) {

                /* TRANSLATORS: the cell battery is very low */
                title = _("Tablet battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Tablet is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);
        } else if (kind == UP_DEVICE_KIND_COMPUTER) {

                /* TRANSLATORS: the cell battery is very low */
                title = _("Attached computer battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Attached computer is very low in power (%.0f%%). "
                                             "The device will soon shutdown if not charged."),
                                           percentage);
        }

        /* close any existing notification of this class */
        notify_close_if_showing (&manager->priv->notification_low);

        /* create a new notification */
        create_notification (title, message,
                             icon_name,
                             &manager->priv->notification_low);
        notify_notification_set_timeout (manager->priv->notification_low,
                                         NOTIFY_EXPIRES_NEVER);
        notify_notification_set_urgency (manager->priv->notification_low,
                                         NOTIFY_URGENCY_CRITICAL);
        notify_notification_set_app_name (manager->priv->notification_low, _("Power"));

        notify_notification_show (manager->priv->notification_low, NULL);

        switch (kind) {

        case UP_DEVICE_KIND_BATTERY:
        case UP_DEVICE_KIND_UPS:
                g_debug ("critical charge level reached, starting sound loop");
                play_loop_start (&manager->priv->critical_alert_timeout_id);
                break;

        default:
                /* play the sound, using sounds from the naming spec */
                ca_context_play (ca_gtk_context_get (), 0,
                                 CA_PROP_EVENT_ID, "battery-caution",
                                 /* TRANSLATORS: this is the sound description */
                                 CA_PROP_EVENT_DESCRIPTION, _("Battery is critically low"), NULL);
                break;
        }
out:
        g_free (icon_name);
        g_free (message);
}

static void
engine_charge_action (GsdPowerManager *manager, UpDevice *device)
{
        const gchar *title = NULL;
        gchar *message = NULL;
        char *icon_name;
        GsdPowerActionType policy;
        guint timer_id;
        UpDeviceKind kind;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      "icon-name", &icon_name,
                      NULL);

        /* check to see if the batteries have not noticed we are on AC */
        if (kind == UP_DEVICE_KIND_BATTERY) {
                if (!up_client_get_on_battery (manager->priv->up_client)) {
                        g_warning ("ignoring critically low message as we are not on battery power");
                        goto out;
                }
        }

        if (kind == UP_DEVICE_KIND_BATTERY) {

                /* TRANSLATORS: laptop battery is really, really, low */
                title = _("Laptop battery critically low");

                /* we have to do different warnings depending on the policy */
                policy = manager_critical_action_get (manager);

                /* use different text for different actions */
                if (policy == GSD_POWER_ACTION_HIBERNATE) {
                        /* TRANSLATORS: computer will hibernate */
                        message = g_strdup (_("The battery is below the critical level and "
                                              "this computer is about to hibernate."));

                } else if (policy == GSD_POWER_ACTION_SHUTDOWN) {
                        /* TRANSLATORS: computer will just shutdown */
                        message = g_strdup (_("The battery is below the critical level and "
                                              "this computer is about to shutdown."));
                }

                /* wait 20 seconds for user-panic */
                timer_id = g_timeout_add_seconds (GSD_ACTION_DELAY,
                                                  (GSourceFunc) manager_critical_action_do_cb,
                                                  manager);
                g_source_set_name_by_id (timer_id, "[GsdPowerManager] battery critical-action");

        } else if (kind == UP_DEVICE_KIND_UPS) {
                /* TRANSLATORS: UPS is really, really, low */
                title = _("UPS critically low");

                /* we have to do different warnings depending on the policy */
                policy = manager_critical_action_get (manager);

                /* use different text for different actions */
                if (policy == GSD_POWER_ACTION_HIBERNATE) {
                        /* TRANSLATORS: computer will hibernate */
                        message = g_strdup (_("UPS is below the critical level and "
                                              "this computer is about to hibernate."));

                } else if (policy == GSD_POWER_ACTION_SHUTDOWN) {
                        /* TRANSLATORS: computer will just shutdown */
                        message = g_strdup (_("UPS is below the critical level and "
                                              "this computer is about to shutdown."));
                }

                /* wait 20 seconds for user-panic */
                timer_id = g_timeout_add_seconds (GSD_ACTION_DELAY,
                                                  (GSourceFunc) manager_critical_action_do_cb,
                                                  manager);
                g_source_set_name_by_id (timer_id, "[GsdPowerManager] ups critical-action");
        }

        /* not all types have actions */
        if (title == NULL)
                return;

        /* close any existing notification of this class */
        notify_close_if_showing (&manager->priv->notification_low);

        /* create a new notification */
        create_notification (title, message,
                             icon_name,
                             &manager->priv->notification_low);
        notify_notification_set_timeout (manager->priv->notification_low,
                                         NOTIFY_EXPIRES_NEVER);
        notify_notification_set_urgency (manager->priv->notification_low,
                                         NOTIFY_URGENCY_CRITICAL);
        notify_notification_set_app_name (manager->priv->notification_low, _("Power"));

        /* try to show */
        notify_notification_show (manager->priv->notification_low, NULL);

        /* play the sound, using sounds from the naming spec */
        ca_context_play (ca_gtk_context_get (), 0,
                         CA_PROP_EVENT_ID, "battery-caution",
                         /* TRANSLATORS: this is the sound description */
                         CA_PROP_EVENT_DESCRIPTION, _("Battery is critically low"), NULL);
out:
        g_free (icon_name);
        g_free (message);
}

static void
engine_device_warning_changed_cb (UpDevice *device, GParamSpec *pspec, GsdPowerManager *manager)
{
        UpDeviceLevel warning;

        g_object_get (device, "warning-level", &warning, NULL);

        if (warning == UP_DEVICE_LEVEL_DISCHARGING) {
                g_debug ("** EMIT: discharging");
                engine_ups_discharging (manager, device);
        } else if (warning == UP_DEVICE_LEVEL_LOW) {
                g_debug ("** EMIT: charge-low");
                engine_charge_low (manager, device);
        } else if (warning == UP_DEVICE_LEVEL_CRITICAL) {
                g_debug ("** EMIT: charge-critical");
                engine_charge_critical (manager, device);
        } else if (warning == UP_DEVICE_LEVEL_ACTION) {
                g_debug ("** EMIT: charge-action");
                engine_charge_action (manager, device);
        } else if (warning == UP_DEVICE_LEVEL_NONE) {
                /* FIXME: this only handles one notification
                 * for the whole system, instead of one per device */
                g_debug ("fully charged or charging, hiding notifications if any");
                notify_close_if_showing (&manager->priv->notification_low);
                notify_close_if_showing (&manager->priv->notification_ups_discharging);
                main_battery_or_ups_low_changed (manager, FALSE);
        }
}

static void
gnome_session_shutdown_cb (GObject *source_object,
                           GAsyncResult *res,
                           gpointer user_data)
{
        GVariant *result;
        GError *error = NULL;

        result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                           res,
                                           &error);
        if (result == NULL) {
                g_warning ("couldn't shutdown using gnome-session: %s",
                           error->message);
                g_error_free (error);
        } else {
                g_variant_unref (result);
        }
}

static void
gnome_session_shutdown (GsdPowerManager *manager)
{
        g_dbus_proxy_call (G_DBUS_PROXY (manager->priv->session),
                           "Shutdown",
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL,
                           gnome_session_shutdown_cb, NULL);
}

static void
gnome_session_logout_cb (GObject *source_object,
                         GAsyncResult *res,
                         gpointer user_data)
{
        GVariant *result;
        GError *error = NULL;

        result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                           res,
                                           &error);
        if (result == NULL) {
                g_warning ("couldn't log out using gnome-session: %s",
                           error->message);
                g_error_free (error);
        } else {
                g_variant_unref (result);
        }
}

static void
gnome_session_logout (GsdPowerManager *manager,
                      guint            logout_mode)
{
        g_dbus_proxy_call (G_DBUS_PROXY (manager->priv->session),
                           "Logout",
                           g_variant_new ("(u)", logout_mode),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL,
                           gnome_session_logout_cb, NULL);
}

static void
action_poweroff (GsdPowerManager *manager)
{
        if (manager->priv->logind_proxy == NULL) {
                g_warning ("no systemd support");
                return;
        }
        g_dbus_proxy_call (manager->priv->logind_proxy,
                           "PowerOff",
                           g_variant_new ("(b)", FALSE),
                           G_DBUS_CALL_FLAGS_NONE,
                           G_MAXINT,
                           NULL,
                           NULL,
                           NULL);
}

static void
action_suspend (GsdPowerManager *manager)
{
        if (manager->priv->logind_proxy == NULL) {
                g_warning ("no systemd support");
                return;
        }
        g_dbus_proxy_call (manager->priv->logind_proxy,
                           "Suspend",
                           g_variant_new ("(b)", FALSE),
                           G_DBUS_CALL_FLAGS_NONE,
                           G_MAXINT,
                           NULL,
                           NULL,
                           NULL);
}

static void
action_hibernate (GsdPowerManager *manager)
{
        if (manager->priv->logind_proxy == NULL) {
                g_warning ("no systemd support");
                return;
        }
        g_dbus_proxy_call (manager->priv->logind_proxy,
                           "Hibernate",
                           g_variant_new ("(b)", FALSE),
                           G_DBUS_CALL_FLAGS_NONE,
                           G_MAXINT,
                           NULL,
                           NULL,
                           NULL);
}

static void
backlight_enable (GsdPowerManager *manager)
{
        gboolean ret;
        GError *error = NULL;

        ret = gnome_rr_screen_set_dpms_mode (manager->priv->rr_screen,
                                             GNOME_RR_DPMS_ON,
                                             &error);
        if (!ret) {
                g_warning ("failed to turn the panel on: %s",
                           error->message);
                g_error_free (error);
        }

        g_debug ("TESTSUITE: Unblanked screen");
}

static void
backlight_disable (GsdPowerManager *manager)
{
        gboolean ret;
        GError *error = NULL;

        ret = gnome_rr_screen_set_dpms_mode (manager->priv->rr_screen,
                                             GNOME_RR_DPMS_OFF,
                                             &error);
        if (!ret) {
                g_warning ("failed to turn the panel off: %s",
                           error->message);
                g_error_free (error);
        }
        g_debug ("TESTSUITE: Blanked screen");
}

static void
do_power_action_type (GsdPowerManager *manager,
                      GsdPowerActionType action_type)
{
        switch (action_type) {
        case GSD_POWER_ACTION_SUSPEND:
                action_suspend (manager);
                break;
        case GSD_POWER_ACTION_INTERACTIVE:
                gnome_session_shutdown (manager);
                break;
        case GSD_POWER_ACTION_HIBERNATE:
                action_hibernate (manager);
                break;
        case GSD_POWER_ACTION_SHUTDOWN:
                /* this is only used on critically low battery where
                 * hibernate is not available and is marginally better
                 * than just powering down the computer mid-write */
                action_poweroff (manager);
                break;
        case GSD_POWER_ACTION_BLANK:
                backlight_disable (manager);
                break;
        case GSD_POWER_ACTION_NOTHING:
                break;
        case GSD_POWER_ACTION_LOGOUT:
                gnome_session_logout (manager, GSM_MANAGER_LOGOUT_MODE_FORCE);
                break;
        }
}

static GsmInhibitorFlag
get_idle_inhibitors_for_action (GsdPowerActionType action_type)
{
        switch (action_type) {
        case GSD_POWER_ACTION_BLANK:
        case GSD_POWER_ACTION_SHUTDOWN:
        case GSD_POWER_ACTION_INTERACTIVE:
                return GSM_INHIBITOR_FLAG_IDLE;
        case GSD_POWER_ACTION_HIBERNATE:
        case GSD_POWER_ACTION_SUSPEND:
                return GSM_INHIBITOR_FLAG_SUSPEND; /* in addition to idle */
        case GSD_POWER_ACTION_NOTHING:
                return 0;
        case GSD_POWER_ACTION_LOGOUT:
                return GSM_INHIBITOR_FLAG_LOGOUT; /* in addition to idle */
        }
        return 0;
}

static gboolean
is_action_inhibited (GsdPowerManager *manager, GsdPowerActionType action_type)
{
        GsmInhibitorFlag flag;
        gboolean is_inhibited;

        flag = get_idle_inhibitors_for_action (action_type);
        if (!flag)
                return FALSE;
        idle_is_session_inhibited (manager,
                                   flag,
                                   &is_inhibited);
        return is_inhibited;
}

static gboolean
upower_kbd_set_brightness (GsdPowerManager *manager, guint value, GError **error)
{
        GVariant *retval;

        /* same as before */
        if (manager->priv->kbd_brightness_now == value)
                return TRUE;

        /* update h/w value */
        retval = g_dbus_proxy_call_sync (manager->priv->upower_kdb_proxy,
                                         "SetBrightness",
                                         g_variant_new ("(i)", (gint) value),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         error);
        if (retval == NULL)
                return FALSE;

        /* save new value */
        manager->priv->kbd_brightness_now = value;
        g_variant_unref (retval);
        return TRUE;
}

static int
upower_kbd_toggle (GsdPowerManager *manager,
                   GError **error)
{
        gboolean ret;
        int value = -1;

        if (manager->priv->kbd_brightness_old >= 0) {
                g_debug ("keyboard toggle off");
                ret = upower_kbd_set_brightness (manager,
                                                 manager->priv->kbd_brightness_old,
                                                 error);
                if (ret) {
                        /* succeeded, set to -1 since now no old value */
                        manager->priv->kbd_brightness_old = -1;
                        value = 0;
                }
        } else {
                g_debug ("keyboard toggle on");
                /* save the current value to restore later when untoggling */
                manager->priv->kbd_brightness_old = manager->priv->kbd_brightness_now;
                ret = upower_kbd_set_brightness (manager, 0, error);
                if (!ret) {
                        /* failed, reset back to -1 */
                        manager->priv->kbd_brightness_old = -1;
                } else {
                        value = 0;
                }
        }

        if (ret)
                return value;
        return -1;
}

static gboolean
suspend_on_lid_close (GsdPowerManager *manager)
{
        GsdXrandrBootBehaviour val;

        if (!external_monitor_is_connected (manager->priv->rr_screen))
                return TRUE;

        val = g_settings_get_enum (manager->priv->settings_xrandr, "default-monitors-setup");
        return val == GSD_XRANDR_BOOT_BEHAVIOUR_DO_NOTHING;
}

static gboolean
inhibit_lid_switch_timer_cb (GsdPowerManager *manager)
{
	/* Just to make sure */
        if (suspend_on_lid_close (manager)) {
                g_debug ("no external monitors for a while; uninhibiting lid close");
                uninhibit_lid_switch (manager);
                manager->priv->inhibit_lid_switch_timer_id = 0;
                return G_SOURCE_REMOVE;
        }

        g_debug ("external monitor still there; trying again later");
        return G_SOURCE_REMOVE;
}

/* Sets up a timer to be triggered some seconds after closing the laptop lid
 * when the laptop is *not* suspended for some reason.  We'll check conditions
 * again in the timeout handler to see if we can suspend then.
 */
static void
setup_inhibit_lid_switch_timer (GsdPowerManager *manager)
{
        if (manager->priv->inhibit_lid_switch_timer_id != 0) {
                g_debug ("lid close safety timer already set up");
                return;
        }

        g_debug ("setting up lid close safety timer");

        manager->priv->inhibit_lid_switch_timer_id = g_timeout_add_seconds (LID_CLOSE_SAFETY_TIMEOUT,
                                                                            (GSourceFunc) inhibit_lid_switch_timer_cb,
                                                                            manager);
        g_source_set_name_by_id (manager->priv->inhibit_lid_switch_timer_id, "[GsdPowerManager] lid close safety timer");
}

static void
stop_inhibit_lid_switch_timer (GsdPowerManager *manager) {
        if (manager->priv->inhibit_lid_switch_timer_id != 0) {
                g_debug ("stopping lid close safety timer");
                g_source_remove (manager->priv->inhibit_lid_switch_timer_id);
                manager->priv->inhibit_lid_switch_timer_id = 0;
        }
}

static void
restart_inhibit_lid_switch_timer (GsdPowerManager *manager)
{
        if (manager->priv->inhibit_lid_switch_timer_id != 0) {
                stop_inhibit_lid_switch_timer (manager);
                g_debug ("restarting lid close safety timer");
                setup_inhibit_lid_switch_timer (manager);
        }
}

static void
do_lid_open_action (GsdPowerManager *manager)
{
        /* play a sound, using sounds from the naming spec */
        ca_context_play (ca_gtk_context_get (), 0,
                         CA_PROP_EVENT_ID, "lid-open",
                         /* TRANSLATORS: this is the sound description */
                         CA_PROP_EVENT_DESCRIPTION, _("Lid has been opened"),
                         NULL);

        /* This might already have happened when resuming, but
         * if we didn't sleep, we'll need to wake it up */
        reset_idletime ();
}

static void
lock_screensaver (GsdPowerManager *manager)
{
        gboolean do_lock;

        do_lock = g_settings_get_boolean (manager->priv->settings_screensaver,
                                          "lock-enabled");
        if (!do_lock) {
                g_dbus_proxy_call_sync (G_DBUS_PROXY (manager->priv->screensaver_proxy),
                                        "SetActive",
                                        g_variant_new ("(b)", TRUE),
                                        G_DBUS_CALL_FLAGS_NONE,
                                        -1, NULL, NULL);
                return;
        }

        g_dbus_proxy_call_sync (G_DBUS_PROXY (manager->priv->screensaver_proxy),
                                "Lock",
                                NULL,
                                G_DBUS_CALL_FLAGS_NONE,
                                -1, NULL, NULL);
}

static void
do_lid_closed_action (GsdPowerManager *manager)
{
        /* play a sound, using sounds from the naming spec */
        ca_context_play (ca_gtk_context_get (), 0,
                         CA_PROP_EVENT_ID, "lid-close",
                         /* TRANSLATORS: this is the sound description */
                         CA_PROP_EVENT_DESCRIPTION, _("Lid has been closed"),
                         NULL);

        /* refresh RANDR so we get an accurate view of what monitors are plugged in when the lid is closed */
        gnome_rr_screen_refresh (manager->priv->rr_screen, NULL); /* NULL-GError */

        if (suspend_on_lid_close (manager)) {
                gboolean is_inhibited;

                idle_is_session_inhibited (manager,
                                           GSM_INHIBITOR_FLAG_SUSPEND,
                                           &is_inhibited);
                if (is_inhibited) {
                        g_debug ("Suspend is inhibited but lid is closed, locking the screen");
                        /* We put the screensaver on * as we're not suspending,
                         * but the lid is closed */
                        lock_screensaver (manager);
                }

                restart_inhibit_lid_switch_timer (manager);
        } else {
                stop_inhibit_lid_switch_timer (manager);
        }
}

static void
up_client_changed_cb (UpClient *client, GsdPowerManager *manager)
{
        gboolean tmp;

        if (!up_client_get_on_battery (client)) {
            /* if we are playing a critical charge sound loop on AC, stop it */
            play_loop_stop (&manager->priv->critical_alert_timeout_id);
            notify_close_if_showing (&manager->priv->notification_low);
            main_battery_or_ups_low_changed (manager, FALSE);
        }

        if (!manager->priv->lid_is_present)
                return;

        /* same lid state */
        tmp = up_client_get_lid_is_closed (manager->priv->up_client);
        if (manager->priv->lid_is_closed == tmp)
                return;
        manager->priv->lid_is_closed = tmp;
        g_debug ("up changed: lid is now %s", tmp ? "closed" : "open");

        if (manager->priv->lid_is_closed)
                do_lid_closed_action (manager);
        else
                do_lid_open_action (manager);
}

static const gchar *
idle_mode_to_string (GsdPowerIdleMode mode)
{
        if (mode == GSD_POWER_IDLE_MODE_NORMAL)
                return "normal";
        if (mode == GSD_POWER_IDLE_MODE_DIM)
                return "dim";
        if (mode == GSD_POWER_IDLE_MODE_BLANK)
                return "blank";
        if (mode == GSD_POWER_IDLE_MODE_SLEEP)
                return "sleep";
        return "unknown";
}

static const char *
idle_watch_id_to_string (GsdPowerManager *manager, guint id)
{
        if (id == manager->priv->idle_dim_id)
                return "dim";
        if (id == manager->priv->idle_blank_id)
                return "blank";
        if (id == manager->priv->idle_sleep_id)
                return "sleep";
        if (id == manager->priv->idle_sleep_warning_id)
                return "sleep-warning";
        return NULL;
}

static void
backlight_iface_emit_changed (GsdPowerManager *manager,
                              const char      *interface_name,
                              gint32           value)
{
        GVariant *params;
        gchar *string;

        /* not yet connected to the bus */
        if (manager->priv->connection == NULL)
                return;

        string = g_strdup_printf ("('%s', [{'Brightness', %%v}], @as [])", interface_name);
        params = g_variant_new_parsed (string,
                                       g_variant_new_int32 (value));
        g_free (string);

        g_dbus_connection_emit_signal (manager->priv->connection,
                                       NULL,
                                       GSD_POWER_DBUS_PATH,
                                       "org.freedesktop.DBus.Properties",
                                       "PropertiesChanged",
                                       params, NULL);
}

static gboolean
display_backlight_dim (GsdPowerManager *manager,
                       gint idle_percentage,
                       GError **error)
{
        gint min;
        gint max;
        gint now;
        gint idle;
        gboolean ret = FALSE;

        if (!manager->priv->backlight_available)
                return TRUE;

        now = backlight_get_abs (manager->priv->rr_screen, error);
        if (now < 0) {
                goto out;
        }

        /* is the dim brightness actually *dimmer* than the
         * brightness we have now? */
        min = backlight_get_min (manager->priv->rr_screen);
        max = backlight_get_max (manager->priv->rr_screen, error);
        if (max < 0) {
                goto out;
        }
        idle = PERCENTAGE_TO_ABS (min, max, idle_percentage);
        if (idle > now) {
                g_debug ("brightness already now %i/%i, so "
                         "ignoring dim to %i/%i",
                         now, max, idle, max);
                ret = TRUE;
                goto out;
        }
        ret = backlight_set_abs (manager->priv->rr_screen,
                                 idle,
                                 error);
        if (!ret) {
                goto out;
        }

        /* save for undim */
        manager->priv->pre_dim_brightness = now;

out:
        return ret;
}

static gboolean
kbd_backlight_dim (GsdPowerManager *manager,
                   gint idle_percentage,
                   GError **error)
{
        gboolean ret;
        gint idle;
        gint max;
        gint now;

        if (manager->priv->upower_kdb_proxy == NULL)
                return TRUE;

        now = manager->priv->kbd_brightness_now;
        max = manager->priv->kbd_brightness_max;
        idle = PERCENTAGE_TO_ABS (0, max, idle_percentage);
        if (idle > now) {
                g_debug ("kbd brightness already now %i/%i, so "
                         "ignoring dim to %i/%i",
                         now, max, idle, max);
                return TRUE;
        }
        ret = upower_kbd_set_brightness (manager, idle, error);
        if (!ret)
                return FALSE;

        /* save for undim */
        manager->priv->kbd_brightness_pre_dim = now;
        return TRUE;
}

static gboolean
is_session_active (GsdPowerManager *manager)
{
        GVariant *variant;
        gboolean is_session_active = FALSE;

        variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (manager->priv->session),
                                                    "SessionIsActive");
        if (variant) {
                is_session_active = g_variant_get_boolean (variant);
                g_variant_unref (variant);
        }

        return is_session_active;
}

static void
idle_set_mode (GsdPowerManager *manager, GsdPowerIdleMode mode)
{
        gboolean ret = FALSE;
        GError *error = NULL;
        gint idle_percentage;
        GsdPowerActionType action_type;
        gboolean is_active = FALSE;

        /* Ignore attempts to set "less idle" modes */
        if (mode <= manager->priv->current_idle_mode &&
            mode != GSD_POWER_IDLE_MODE_NORMAL) {
                g_debug ("Not going to 'less idle' mode %s (current: %s)",
                         idle_mode_to_string (mode),
                         idle_mode_to_string (manager->priv->current_idle_mode));
                return;
        }

        /* ensure we're still on an active console */
        is_active = is_session_active (manager);
        if (!is_active) {
                g_debug ("ignoring state transition to %s as inactive",
                         idle_mode_to_string (mode));
                return;
        }

        /* don't do any power saving if we're a VM */
        if (manager->priv->is_virtual_machine) {
                g_debug ("ignoring state transition to %s as virtual machine",
                         idle_mode_to_string (mode));
                return;
        }

        manager->priv->current_idle_mode = mode;
        g_debug ("Doing a state transition: %s", idle_mode_to_string (mode));

        /* if we're moving to an idle mode, make sure
         * we add a watch to take us back to normal */
        if (mode != GSD_POWER_IDLE_MODE_NORMAL) {
                gnome_idle_monitor_add_user_active_watch (manager->priv->idle_monitor,
                                                          idle_became_active_cb,
                                                          manager,
                                                          NULL);
        }

        /* save current brightness, and set dim level */
        if (mode == GSD_POWER_IDLE_MODE_DIM) {
                /* display backlight */
                idle_percentage = g_settings_get_int (manager->priv->settings,
                                                      "idle-brightness");
                ret = display_backlight_dim (manager, idle_percentage, &error);
                if (!ret) {
                        g_warning ("failed to set dim backlight to %i%%: %s",
                                   idle_percentage,
                                   error->message);
                        g_clear_error (&error);
                }

                /* keyboard backlight */
                ret = kbd_backlight_dim (manager, idle_percentage, &error);
                if (!ret) {
                        g_warning ("failed to set dim kbd backlight to %i%%: %s",
                                   idle_percentage,
                                   error->message);
                        g_clear_error (&error);
                }

        /* turn off screen and kbd */
        } else if (mode == GSD_POWER_IDLE_MODE_BLANK) {

                backlight_disable (manager);

                /* only toggle keyboard if present and not already toggled */
                if (manager->priv->upower_kdb_proxy &&
                    manager->priv->kbd_brightness_old == -1) {
                        if (upower_kbd_toggle (manager, &error) < 0) {
                                g_warning ("failed to turn the kbd backlight off: %s",
                                           error->message);
                                g_error_free (error);
                        }
                }

        /* sleep */
        } else if (mode == GSD_POWER_IDLE_MODE_SLEEP) {

                if (up_client_get_on_battery (manager->priv->up_client)) {
                        action_type = g_settings_get_enum (manager->priv->settings,
                                                           "sleep-inactive-battery-type");
                } else {
                        action_type = g_settings_get_enum (manager->priv->settings,
                                                           "sleep-inactive-ac-type");
                }
                do_power_action_type (manager, action_type);

        /* turn on screen and restore user-selected brightness level */
        } else if (mode == GSD_POWER_IDLE_MODE_NORMAL) {

                backlight_enable (manager);

                /* reset brightness if we dimmed */
                if (manager->priv->pre_dim_brightness >= 0) {
                        ret = backlight_set_abs (manager->priv->rr_screen,
                                                 manager->priv->pre_dim_brightness,
                                                 &error);
                        if (!ret) {
                                g_warning ("failed to restore backlight to %i: %s",
                                           manager->priv->pre_dim_brightness,
                                           error->message);
                                g_clear_error (&error);
                        } else {
                                manager->priv->pre_dim_brightness = -1;
                        }
                }

                /* only toggle keyboard if present and already toggled off */
                if (manager->priv->upower_kdb_proxy &&
                    manager->priv->kbd_brightness_old != -1) {
                        if (upower_kbd_toggle (manager, &error) < 0) {
                                g_warning ("failed to turn the kbd backlight on: %s",
                                           error->message);
                                g_clear_error (&error);
                        }
                }

                /* reset kbd brightness if we dimmed */
                if (manager->priv->kbd_brightness_pre_dim >= 0) {
                        ret = upower_kbd_set_brightness (manager,
                                                         manager->priv->kbd_brightness_pre_dim,
                                                         &error);
                        if (!ret) {
                                g_warning ("failed to restore kbd backlight to %i: %s",
                                           manager->priv->kbd_brightness_pre_dim,
                                           error->message);
                                g_error_free (error);
                        }
                        manager->priv->kbd_brightness_pre_dim = -1;
                }

        }
}

static gboolean
idle_is_session_inhibited (GsdPowerManager  *manager,
                           GsmInhibitorFlag  mask,
                           gboolean         *is_inhibited)
{
        GVariant *variant;
        GsmInhibitorFlag inhibited_actions;

        /* not yet connected to gnome-session */
        if (manager->priv->session == NULL)
                return FALSE;

        variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (manager->priv->session),
                                                    "InhibitedActions");
        if (!variant)
                return FALSE;

        inhibited_actions = g_variant_get_uint32 (variant);
        g_variant_unref (variant);

        *is_inhibited = (inhibited_actions & mask);

        return TRUE;
}

static void
clear_idle_watch (GnomeIdleMonitor *monitor,
                  guint            *id)
{
        if (*id == 0)
                return;
        gnome_idle_monitor_remove_watch (monitor, *id);
        *id = 0;
}

static void
idle_configure (GsdPowerManager *manager)
{
        gboolean is_idle_inhibited;
        GsdPowerActionType action_type;
        guint timeout_blank;
        guint timeout_sleep;
        guint timeout_dim;
        gboolean on_battery;

        if (!idle_is_session_inhibited (manager,
                                        GSM_INHIBITOR_FLAG_IDLE,
                                        &is_idle_inhibited)) {
                /* Session isn't available yet, postpone */
                return;
        }

        /* are we inhibited from going idle */
        if (!is_session_active (manager) || is_idle_inhibited) {
                if (is_idle_inhibited)
                        g_debug ("inhibited, so using normal state");
                else
                        g_debug ("inactive, so using normal state");
                idle_set_mode (manager, GSD_POWER_IDLE_MODE_NORMAL);

                clear_idle_watch (manager->priv->idle_monitor,
                                  &manager->priv->idle_blank_id);
                clear_idle_watch (manager->priv->idle_monitor,
                                  &manager->priv->idle_sleep_id);
                clear_idle_watch (manager->priv->idle_monitor,
                                  &manager->priv->idle_dim_id);
                clear_idle_watch (manager->priv->idle_monitor,
                                  &manager->priv->idle_sleep_warning_id);
                notify_close_if_showing (&manager->priv->notification_sleep_warning);
                return;
        }

        /* set up blank callback only when the screensaver is on,
         * as it's what will drive the blank */
        timeout_blank = 0;
        if (manager->priv->screensaver_active) {
                /* The tail is wagging the dog.
                 * The screensaver coming on will blank the screen.
                 * If an event occurs while the screensaver is on,
                 * the aggressive idle watch will handle it */
                timeout_blank = SCREENSAVER_TIMEOUT_BLANK;
        }

        clear_idle_watch (manager->priv->idle_monitor,
                          &manager->priv->idle_blank_id);

        if (timeout_blank != 0) {
                g_debug ("setting up blank callback for %is", timeout_blank);

                manager->priv->idle_blank_id = gnome_idle_monitor_add_idle_watch (manager->priv->idle_monitor,
                                                                                  timeout_blank * 1000,
                                                                                  idle_triggered_idle_cb, manager, NULL);
        }

        /* only do the sleep timeout when the session is idle
         * and we aren't inhibited from sleeping (or logging out, etc.) */
        on_battery = up_client_get_on_battery (manager->priv->up_client);
        action_type = g_settings_get_enum (manager->priv->settings, on_battery ?
                                           "sleep-inactive-battery-type" : "sleep-inactive-ac-type");
        timeout_sleep = 0;
        if (!is_action_inhibited (manager, action_type)) {
                timeout_sleep = g_settings_get_int (manager->priv->settings, on_battery ?
                                                    "sleep-inactive-battery-timeout" : "sleep-inactive-ac-timeout");
        }

        clear_idle_watch (manager->priv->idle_monitor,
                          &manager->priv->idle_sleep_id);
        clear_idle_watch (manager->priv->idle_monitor,
                          &manager->priv->idle_sleep_warning_id);

        if (timeout_sleep != 0) {
                g_debug ("setting up sleep callback %is", timeout_sleep);

                manager->priv->idle_sleep_id = gnome_idle_monitor_add_idle_watch (manager->priv->idle_monitor,
                                                                                  timeout_sleep * 1000,
                                                                                  idle_triggered_idle_cb, manager, NULL);
                if (action_type == GSD_POWER_ACTION_LOGOUT ||
                    action_type == GSD_POWER_ACTION_SUSPEND ||
                    action_type == GSD_POWER_ACTION_HIBERNATE) {
                        guint timeout_sleep_warning;

                        manager->priv->sleep_action_type = action_type;
                        timeout_sleep_warning = timeout_sleep * IDLE_DELAY_TO_IDLE_DIM_MULTIPLIER;
                        if (timeout_sleep_warning < MINIMUM_IDLE_DIM_DELAY)
                                timeout_sleep_warning = 0;

                        g_debug ("setting up sleep warning callback %is", timeout_sleep_warning);

                        manager->priv->idle_sleep_warning_id = gnome_idle_monitor_add_idle_watch (manager->priv->idle_monitor,
                                                                                                  timeout_sleep_warning * 1000,
                                                                                                  idle_triggered_idle_cb, manager, NULL);
                }
        }

        if (manager->priv->idle_sleep_warning_id == 0)
                notify_close_if_showing (&manager->priv->notification_sleep_warning);

        /* set up dim callback for when the screen lock is not active,
         * but only if we actually want to dim. */
        timeout_dim = 0;
        if (manager->priv->screensaver_active) {
                /* Don't dim when the screen lock is active */
        } else if (!on_battery) {
                /* Don't dim when charging */
        } else if (manager->priv->battery_is_low) {
                /* Aggressively blank when battery is low */
                timeout_dim = SCREENSAVER_TIMEOUT_BLANK;
        } else {
                if (g_settings_get_boolean (manager->priv->settings, "idle-dim")) {
                        timeout_dim = g_settings_get_uint (manager->priv->settings_bus,
                                                           "idle-delay");
                        if (timeout_dim == 0) {
                                timeout_dim = IDLE_DIM_BLANK_DISABLED_MIN;
                        } else {
                                timeout_dim *= IDLE_DELAY_TO_IDLE_DIM_MULTIPLIER;
                                /* Don't bother dimming if the idle-delay is
                                 * too low, we'll do that when we bring down the
                                 * screen lock */
                                if (timeout_dim < MINIMUM_IDLE_DIM_DELAY)
                                        timeout_dim = 0;
                        }
                }
        }

        clear_idle_watch (manager->priv->idle_monitor,
                          &manager->priv->idle_dim_id);

        if (timeout_dim != 0) {
                g_debug ("setting up dim callback for %is", timeout_dim);

                manager->priv->idle_dim_id = gnome_idle_monitor_add_idle_watch (manager->priv->idle_monitor,
                                                                                timeout_dim * 1000,
                                                                                idle_triggered_idle_cb, manager, NULL);
        }
}

static void
main_battery_or_ups_low_changed (GsdPowerManager *manager,
                          gboolean         is_low)
{
        if (is_low == manager->priv->battery_is_low)
                return;
        manager->priv->battery_is_low = is_low;
        idle_configure (manager);
}

static gboolean
temporary_unidle_done_cb (GsdPowerManager *manager)
{
        idle_set_mode (manager, manager->priv->previous_idle_mode);
        manager->priv->temporary_unidle_on_ac_id = 0;
        return FALSE;
}

static void
set_temporary_unidle_on_ac (GsdPowerManager *manager,
                            gboolean         enable)
{
        if (!enable) {
                if (manager->priv->temporary_unidle_on_ac_id != 0) {
                        g_source_remove (manager->priv->temporary_unidle_on_ac_id);
                        manager->priv->temporary_unidle_on_ac_id = 0;

                        idle_set_mode (manager, manager->priv->previous_idle_mode);
                }
        } else {
                /* Don't overwrite the previous idle mode when an unidle is
                 * already on-going */
                if (manager->priv->temporary_unidle_on_ac_id != 0) {
                        g_source_remove (manager->priv->temporary_unidle_on_ac_id);
                } else {
                        manager->priv->previous_idle_mode = manager->priv->current_idle_mode;
                        idle_set_mode (manager, GSD_POWER_IDLE_MODE_NORMAL);
                }
                manager->priv->temporary_unidle_on_ac_id = g_timeout_add_seconds (POWER_UP_TIME_ON_AC,
                                                                                  (GSourceFunc) temporary_unidle_done_cb,
                                                                                  manager);
        }
}

static void
up_client_on_battery_cb (UpClient *client,
                         GParamSpec *pspec,
                         GsdPowerManager *manager)
{
        idle_configure (manager);

        if (manager->priv->lid_is_closed)
                return;

        if (manager->priv->current_idle_mode == GSD_POWER_IDLE_MODE_BLANK ||
            manager->priv->current_idle_mode == GSD_POWER_IDLE_MODE_DIM ||
            manager->priv->temporary_unidle_on_ac_id != 0)
                set_temporary_unidle_on_ac (manager, TRUE);
}

static void
gsd_power_manager_finalize (GObject *object)
{
        GsdPowerManager *manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GSD_IS_POWER_MANAGER (object));

        manager = GSD_POWER_MANAGER (object);

        g_return_if_fail (manager->priv != NULL);

        g_clear_object (&manager->priv->connection);

        if (manager->priv->name_id != 0)
                g_bus_unown_name (manager->priv->name_id);

        G_OBJECT_CLASS (gsd_power_manager_parent_class)->finalize (object);
}

static void
gsd_power_manager_class_init (GsdPowerManagerClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = gsd_power_manager_finalize;

        g_type_class_add_private (klass, sizeof (GsdPowerManagerPrivate));
}

static void
session_presence_proxy_ready_cb (GObject *source_object,
                                 GAsyncResult *res,
                                 gpointer user_data)
{
        GError *error = NULL;
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        manager->priv->session_presence_proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (manager->priv->session_presence_proxy == NULL) {
                g_warning ("Could not connect to gnome-sesson: %s",
                           error->message);
                g_error_free (error);
                return;
        }
}

static void
handle_screensaver_active (GsdPowerManager *manager,
                           GVariant        *parameters)
{
        gboolean active;

        g_variant_get (parameters, "(b)", &active);
        g_debug ("Received screensaver ActiveChanged signal: %d (old: %d)", active, manager->priv->screensaver_active);
        if (manager->priv->screensaver_active != active) {
                manager->priv->screensaver_active = active;
                idle_configure (manager);

                /* Setup blank as soon as the screensaver comes on,
                 * and its fade has finished.
                 *
                 * See also idle_configure() */
                if (active)
                        idle_set_mode (manager, GSD_POWER_IDLE_MODE_BLANK);
        }
}

static void
screensaver_signal_cb (GDBusProxy *proxy,
                       const gchar *sender_name,
                       const gchar *signal_name,
                       GVariant *parameters,
                       gpointer user_data)
{
        if (g_strcmp0 (signal_name, "ActiveChanged") == 0)
                handle_screensaver_active (GSD_POWER_MANAGER (user_data), parameters);
}

static void
power_keyboard_proxy_ready_cb (GObject             *source_object,
                               GAsyncResult        *res,
                               gpointer             user_data)
{
        GVariant *k_now = NULL;
        GVariant *k_max = NULL;
        GError *error = NULL;
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        manager->priv->upower_kdb_proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (manager->priv->upower_kdb_proxy == NULL) {
                g_warning ("Could not connect to UPower: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }

        k_now = g_dbus_proxy_call_sync (manager->priv->upower_kdb_proxy,
                                        "GetBrightness",
                                        NULL,
                                        G_DBUS_CALL_FLAGS_NONE,
                                        -1,
                                        NULL,
                                        &error);
        if (k_now == NULL) {
                if (error->domain != G_DBUS_ERROR ||
                    error->code != G_DBUS_ERROR_UNKNOWN_METHOD) {
                        g_warning ("Failed to get brightness: %s",
                                   error->message);
                } else {
                        /* Keyboard brightness is not available */
                        g_clear_object (&manager->priv->upower_kdb_proxy);
                }
                g_error_free (error);
                goto out;
        }

        k_max = g_dbus_proxy_call_sync (manager->priv->upower_kdb_proxy,
                                        "GetMaxBrightness",
                                        NULL,
                                        G_DBUS_CALL_FLAGS_NONE,
                                        -1,
                                        NULL,
                                        &error);
        if (k_max == NULL) {
                g_warning ("Failed to get max brightness: %s", error->message);
                g_error_free (error);
                goto out;
        }

        g_variant_get (k_now, "(i)", &manager->priv->kbd_brightness_now);
        g_variant_get (k_max, "(i)", &manager->priv->kbd_brightness_max);

        /* set brightness to max if not currently set so is something
         * sensible */
        if (manager->priv->kbd_brightness_now <= 0) {
                gboolean ret;
                ret = upower_kbd_set_brightness (manager,
                                                 manager->priv->kbd_brightness_max,
                                                 &error);
                if (!ret) {
                        g_warning ("failed to initialize kbd backlight to %i: %s",
                                   manager->priv->kbd_brightness_max,
                                   error->message);
                        g_error_free (error);
                }
        }

        /* Tell the front-end that the brightness changed from
         * its default "-1/no keyboard backlight available" default */
        backlight_iface_emit_changed (manager, GSD_POWER_DBUS_INTERFACE_KEYBOARD, manager->priv->kbd_brightness_now);

out:
        if (k_now != NULL)
                g_variant_unref (k_now);
        if (k_max != NULL)
                g_variant_unref (k_max);
}

static void
show_sleep_warning (GsdPowerManager *manager)
{
        /* close any existing notification of this class */
        notify_close_if_showing (&manager->priv->notification_sleep_warning);

        /* create a new notification */
        switch (manager->priv->sleep_action_type) {
        case GSD_POWER_ACTION_LOGOUT:
                create_notification (_("Automatic logout"), _("You will soon log out because of inactivity."),
                                     NULL,
                                     &manager->priv->notification_sleep_warning);
                break;
        case GSD_POWER_ACTION_SUSPEND:
                create_notification (_("Automatic suspend"), _("Computer will suspend very soon because of inactivity."),
                                     NULL,
                                     &manager->priv->notification_sleep_warning);
                break;
        case GSD_POWER_ACTION_HIBERNATE:
                create_notification (_("Automatic hibernation"), _("Computer will suspend very soon because of inactivity."),
                                     NULL,
                                     &manager->priv->notification_sleep_warning);
                break;
        default:
                g_assert_not_reached ();
                break;
        }
        notify_notification_set_timeout (manager->priv->notification_sleep_warning,
                                         NOTIFY_EXPIRES_NEVER);
        notify_notification_set_urgency (manager->priv->notification_sleep_warning,
                                         NOTIFY_URGENCY_CRITICAL);
        notify_notification_set_app_name (manager->priv->notification_sleep_warning, _("Power"));

        notify_notification_show (manager->priv->notification_sleep_warning, NULL);

        if (manager->priv->sleep_action_type == GSD_POWER_ACTION_LOGOUT)
                set_temporary_unidle_on_ac (manager, TRUE);
}

static void
idle_triggered_idle_cb (GnomeIdleMonitor *monitor,
                        guint             watch_id,
                        gpointer          user_data)
{
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);
        const char *id_name;

        id_name = idle_watch_id_to_string (manager, watch_id);
        if (id_name == NULL)
                g_debug ("idletime watch: %i", watch_id);
        else
                g_debug ("idletime watch: %s (%i)", id_name, watch_id);

        if (watch_id == manager->priv->idle_dim_id) {
                idle_set_mode (manager, GSD_POWER_IDLE_MODE_DIM);
        } else if (watch_id == manager->priv->idle_blank_id) {
                idle_set_mode (manager, GSD_POWER_IDLE_MODE_BLANK);
        } else if (watch_id == manager->priv->idle_sleep_id) {
                idle_set_mode (manager, GSD_POWER_IDLE_MODE_SLEEP);
        } else if (watch_id == manager->priv->idle_sleep_warning_id) {
                show_sleep_warning (manager);
        }
}

static void
idle_became_active_cb (GnomeIdleMonitor *monitor,
                       guint             watch_id,
                       gpointer          user_data)
{
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        g_debug ("idletime reset");

        set_temporary_unidle_on_ac (manager, FALSE);

        /* close any existing notification about idleness */
        notify_close_if_showing (&manager->priv->notification_sleep_warning);

        idle_set_mode (manager, GSD_POWER_IDLE_MODE_NORMAL);
}

static void
engine_settings_key_changed_cb (GSettings *settings,
                                const gchar *key,
                                GsdPowerManager *manager)
{
        if (g_str_has_prefix (key, "sleep-inactive") ||
            g_str_equal (key, "idle-delay") ||
            g_str_equal (key, "idle-dim")) {
                idle_configure (manager);
                return;
        }
}

static void
engine_session_properties_changed_cb (GDBusProxy      *session,
                                      GVariant        *changed,
                                      char           **invalidated,
                                      GsdPowerManager *manager)
{
        GVariant *v;

        v = g_variant_lookup_value (changed, "SessionIsActive", G_VARIANT_TYPE_BOOLEAN);
        if (v) {
                gboolean active;

                active = g_variant_get_boolean (v);
                g_debug ("Received session is active change: now %s", active ? "active" : "inactive");
                /* when doing the fast-user-switch into a new account,
                 * ensure the new account is undimmed and with the backlight on */
                if (active)
                        idle_set_mode (manager, GSD_POWER_IDLE_MODE_NORMAL);
                g_variant_unref (v);

        }

        v = g_variant_lookup_value (changed, "InhibitedActions", G_VARIANT_TYPE_UINT32);
        if (v) {
                g_variant_unref (v);
                g_debug ("Received gnome session inhibitor change");
                idle_configure (manager);
        }
}

static void
inhibit_lid_switch_done (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
        GDBusProxy *proxy = G_DBUS_PROXY (source);
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);
        GError *error = NULL;
        GVariant *res;
        GUnixFDList *fd_list = NULL;
        gint idx;

        res = g_dbus_proxy_call_with_unix_fd_list_finish (proxy, &fd_list, result, &error);
        if (res == NULL) {
                g_warning ("Unable to inhibit lid switch: %s", error->message);
                g_error_free (error);
        } else {
                g_variant_get (res, "(h)", &idx);
                manager->priv->inhibit_lid_switch_fd = g_unix_fd_list_get (fd_list, idx, &error);
                if (manager->priv->inhibit_lid_switch_fd == -1) {
                        g_warning ("Failed to receive system inhibitor fd: %s", error->message);
                        g_error_free (error);
                }
                g_debug ("System inhibitor fd is %d", manager->priv->inhibit_lid_switch_fd);
                g_object_unref (fd_list);
                g_variant_unref (res);
        }
}

static void
inhibit_lid_switch (GsdPowerManager *manager)
{
        GVariant *params;

        if (manager->priv->inhibit_lid_switch_taken) {
                g_debug ("already inhibited lid-switch");
                return;
        }
        g_debug ("Adding lid switch system inhibitor");
        manager->priv->inhibit_lid_switch_taken = TRUE;

        params = g_variant_new ("(ssss)",
                                "handle-lid-switch",
                                g_get_user_name (),
                                "Multiple displays attached",
                                "block");
        g_dbus_proxy_call_with_unix_fd_list (manager->priv->logind_proxy,
                                             "Inhibit",
                                             params,
                                             0,
                                             G_MAXINT,
                                             NULL,
                                             NULL,
                                             inhibit_lid_switch_done,
                                             manager);
}

static void
uninhibit_lid_switch (GsdPowerManager *manager)
{
        if (manager->priv->inhibit_lid_switch_fd == -1) {
                g_debug ("no lid-switch inhibitor");
                return;
        }
        g_debug ("Removing lid switch system inhibitor");
        close (manager->priv->inhibit_lid_switch_fd);
        manager->priv->inhibit_lid_switch_fd = -1;
        manager->priv->inhibit_lid_switch_taken = FALSE;
}

static void
inhibit_suspend_done (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
        GDBusProxy *proxy = G_DBUS_PROXY (source);
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);
        GError *error = NULL;
        GVariant *res;
        GUnixFDList *fd_list = NULL;
        gint idx;

        res = g_dbus_proxy_call_with_unix_fd_list_finish (proxy, &fd_list, result, &error);
        if (res == NULL) {
                g_warning ("Unable to inhibit suspend: %s", error->message);
                g_error_free (error);
        } else {
                g_variant_get (res, "(h)", &idx);
                manager->priv->inhibit_suspend_fd = g_unix_fd_list_get (fd_list, idx, &error);
                if (manager->priv->inhibit_suspend_fd == -1) {
                        g_warning ("Failed to receive system inhibitor fd: %s", error->message);
                        g_error_free (error);
                }
                g_debug ("System inhibitor fd is %d", manager->priv->inhibit_suspend_fd);
                g_object_unref (fd_list);
                g_variant_unref (res);
        }
}

/* We take a delay inhibitor here, which causes logind to send a
 * PrepareForSleep signal, which gives us a chance to lock the screen
 * and do some other preparations.
 */
static void
inhibit_suspend (GsdPowerManager *manager)
{
        if (manager->priv->inhibit_suspend_taken) {
                g_debug ("already inhibited lid-switch");
                return;
        }
        g_debug ("Adding suspend delay inhibitor");
        manager->priv->inhibit_suspend_taken = TRUE;
        g_dbus_proxy_call_with_unix_fd_list (manager->priv->logind_proxy,
                                             "Inhibit",
                                             g_variant_new ("(ssss)",
                                                            "sleep",
                                                            g_get_user_name (),
                                                            "GNOME needs to lock the screen",
                                                            "delay"),
                                             0,
                                             G_MAXINT,
                                             NULL,
                                             NULL,
                                             inhibit_suspend_done,
                                             manager);
}

static void
uninhibit_suspend (GsdPowerManager *manager)
{
        if (manager->priv->inhibit_suspend_fd == -1) {
                g_debug ("no suspend delay inhibitor");
                return;
        }
        g_debug ("Removing suspend delay inhibitor");
        close (manager->priv->inhibit_suspend_fd);
        manager->priv->inhibit_suspend_fd = -1;
        manager->priv->inhibit_suspend_taken = FALSE;
}

static void
on_randr_event (GnomeRRScreen *screen, gpointer user_data)
{
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        g_debug ("Screen configuration changed");

        if (suspend_on_lid_close (manager)) {
                restart_inhibit_lid_switch_timer (manager);
                return;
        }

        /* when a second monitor is plugged in, we take the
         * handle-lid-switch inhibitor lock of logind to prevent
         * it from suspending.
         *
         * Uninhibiting is done in the inhibit_lid_switch_timer,
         * since we want to give users a few seconds when unplugging
         * and replugging an external monitor, not suspend right away.
         */
        inhibit_lid_switch (manager);
        setup_inhibit_lid_switch_timer (manager);
}

static void
handle_suspend_actions (GsdPowerManager *manager)
{
        backlight_disable (manager);
        uninhibit_suspend (manager);
}

static void
handle_resume_actions (GsdPowerManager *manager)
{
        /* close existing notifications on resume, the system power
         * state is probably different now */
        notify_close_if_showing (&manager->priv->notification_low);
        notify_close_if_showing (&manager->priv->notification_ups_discharging);
        main_battery_or_ups_low_changed (manager, FALSE);

        /* ensure we turn the panel back on after resume */
        backlight_enable (manager);

        /* And work-around Xorg bug:
         * https://bugs.freedesktop.org/show_bug.cgi?id=59576 */
        reset_idletime ();

        /* set up the delay again */
        inhibit_suspend (manager);
}

static void
logind_proxy_signal_cb (GDBusProxy  *proxy,
                        const gchar *sender_name,
                        const gchar *signal_name,
                        GVariant    *parameters,
                        gpointer     user_data)
{
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);
        gboolean is_about_to_suspend;

        if (g_strcmp0 (signal_name, "PrepareForSleep") != 0)
                return;
        g_variant_get (parameters, "(b)", &is_about_to_suspend);
        if (is_about_to_suspend) {
                handle_suspend_actions (manager);
        } else {
                handle_resume_actions (manager);
        }
}

static void
on_rr_screen_acquired (GObject      *object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
        GsdPowerManager *manager = user_data;

        gnome_settings_profile_start (NULL);

        manager->priv->rr_screen = gnome_rr_screen_new_finish (result, NULL);

        /* set up the screens */
        if (manager->priv->lid_is_present) {
                g_signal_connect (manager->priv->rr_screen, "changed", G_CALLBACK (on_randr_event), manager);
                watch_external_monitor (manager->priv->rr_screen);
                on_randr_event (manager->priv->rr_screen, manager);
        }

        /* check whether a backlight is available */
        manager->priv->backlight_available = backlight_available (manager->priv->rr_screen);

        /* ensure the default dpms timeouts are cleared */
        backlight_enable (manager);

        g_signal_connect (manager->priv->logind_proxy, "g-signal",
                          G_CALLBACK (logind_proxy_signal_cb),
                          manager);
        /* Set up a delay inhibitor to be informed about suspend attempts */
        inhibit_suspend (manager);

        /* track the active session */
        manager->priv->session = gnome_settings_bus_get_session_proxy ();
        g_signal_connect (manager->priv->session, "g-properties-changed",
                          G_CALLBACK (engine_session_properties_changed_cb),
                          manager);

        manager->priv->screensaver_proxy = gnome_settings_bus_get_screen_saver_proxy ();

        g_signal_connect (manager->priv->screensaver_proxy, "g-signal",
                          G_CALLBACK (screensaver_signal_cb), manager);

        manager->priv->kbd_brightness_old = -1;
        manager->priv->kbd_brightness_pre_dim = -1;
        manager->priv->pre_dim_brightness = -1;
        g_signal_connect (manager->priv->settings, "changed",
                          G_CALLBACK (engine_settings_key_changed_cb), manager);
        g_signal_connect (manager->priv->settings_bus, "changed",
                          G_CALLBACK (engine_settings_key_changed_cb), manager);
        g_signal_connect (manager->priv->up_client, "device-added",
                          G_CALLBACK (engine_device_added_cb), manager);
        g_signal_connect (manager->priv->up_client, "device-removed",
                          G_CALLBACK (engine_device_removed_cb), manager);
        g_signal_connect_after (manager->priv->up_client, "changed",
                                G_CALLBACK (up_client_changed_cb), manager);
        g_signal_connect (manager->priv->up_client, "notify::on-battery",
                          G_CALLBACK (up_client_on_battery_cb), manager);

        /* connect to UPower for keyboard backlight control */
        manager->priv->kbd_brightness_now = -1;
        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                  NULL,
                                  UPOWER_DBUS_NAME,
                                  UPOWER_DBUS_PATH_KBDBACKLIGHT,
                                  UPOWER_DBUS_INTERFACE_KBDBACKLIGHT,
                                  NULL,
                                  power_keyboard_proxy_ready_cb,
                                  manager);

        /* connect to the session */
        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                  0,
                                  NULL,
                                  GNOME_SESSION_DBUS_NAME,
                                  GNOME_SESSION_DBUS_PATH_PRESENCE,
                                  GNOME_SESSION_DBUS_INTERFACE_PRESENCE,
                                  NULL,
                                  session_presence_proxy_ready_cb,
                                  manager);

        manager->priv->devices_array = g_ptr_array_new_with_free_func (g_object_unref);

        /* create a fake virtual composite battery */
        manager->priv->device_composite = up_client_get_display_device (manager->priv->up_client);
        g_signal_connect (manager->priv->device_composite, "notify::warning-level",
                          G_CALLBACK (engine_device_warning_changed_cb), manager);

        /* create IDLETIME watcher */
        manager->priv->idle_monitor = gnome_idle_monitor_new ();

        /* coldplug the engine */
        engine_coldplug (manager);
        idle_configure (manager);

        manager->priv->xscreensaver_watchdog_timer_id = gsd_power_enable_screensaver_watchdog ();

        /* don't blank inside a VM */
        manager->priv->is_virtual_machine = gsd_power_is_hardware_a_vm ();

        gnome_settings_profile_end (NULL);
}

gboolean
gsd_power_manager_start (GsdPowerManager *manager,
                         GError **error)
{
        g_debug ("Starting power manager");
        gnome_settings_profile_start (NULL);

        /* Check whether we have a lid first */
        manager->priv->up_client = up_client_new ();
        manager->priv->lid_is_present = up_client_get_lid_is_present (manager->priv->up_client);
        if (manager->priv->lid_is_present)
                manager->priv->lid_is_closed = up_client_get_lid_is_closed (manager->priv->up_client);

        /* Set up the logind proxy */
        manager->priv->logind_proxy =
                g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                               0,
                                               NULL,
                                               SYSTEMD_DBUS_NAME,
                                               SYSTEMD_DBUS_PATH,
                                               SYSTEMD_DBUS_INTERFACE,
                                               NULL,
                                               error);
        if (manager->priv->logind_proxy == NULL) {
                g_debug ("No systemd (logind) support, disabling plugin");
                return FALSE;
        }

        /* Check for XTEST support */
        if (supports_xtest () == FALSE) {
                g_debug ("XTEST extension required, disabling plugin");
                return FALSE;
        }

        /* coldplug the list of screens */
        gnome_rr_screen_new_async (gdk_screen_get_default (),
                                   on_rr_screen_acquired, manager);

        manager->priv->settings = g_settings_new (GSD_POWER_SETTINGS_SCHEMA);
        manager->priv->settings_screensaver = g_settings_new ("org.gnome.desktop.screensaver");
        manager->priv->settings_bus = g_settings_new ("org.gnome.desktop.session");
        manager->priv->settings_xrandr = g_settings_new (GSD_XRANDR_SETTINGS_SCHEMA);

        gnome_settings_profile_end (NULL);
        return TRUE;
}

void
gsd_power_manager_stop (GsdPowerManager *manager)
{
        g_debug ("Stopping power manager");

        if (manager->priv->inhibit_lid_switch_timer_id != 0) {
                g_source_remove (manager->priv->inhibit_lid_switch_timer_id);
                manager->priv->inhibit_lid_switch_timer_id = 0;
        }

        if (manager->priv->bus_cancellable != NULL) {
                g_cancellable_cancel (manager->priv->bus_cancellable);
                g_object_unref (manager->priv->bus_cancellable);
                manager->priv->bus_cancellable = NULL;
        }

        if (manager->priv->introspection_data) {
                g_dbus_node_info_unref (manager->priv->introspection_data);
                manager->priv->introspection_data = NULL;
        }

        g_signal_handlers_disconnect_by_data (manager->priv->up_client, manager);

        g_clear_object (&manager->priv->session);
        g_clear_object (&manager->priv->settings);
        g_clear_object (&manager->priv->settings_screensaver);
        g_clear_object (&manager->priv->settings_bus);
        g_clear_object (&manager->priv->up_client);

        if (manager->priv->inhibit_lid_switch_fd != -1) {
                close (manager->priv->inhibit_lid_switch_fd);
                manager->priv->inhibit_lid_switch_fd = -1;
                manager->priv->inhibit_lid_switch_taken = FALSE;
        }
        if (manager->priv->inhibit_suspend_fd != -1) {
                close (manager->priv->inhibit_suspend_fd);
                manager->priv->inhibit_suspend_fd = -1;
                manager->priv->inhibit_suspend_taken = FALSE;
        }

        g_clear_object (&manager->priv->logind_proxy);
        g_clear_object (&manager->priv->rr_screen);

        g_ptr_array_unref (manager->priv->devices_array);
        manager->priv->devices_array = NULL;
        g_clear_object (&manager->priv->device_composite);

        g_clear_object (&manager->priv->session_presence_proxy);
        g_clear_object (&manager->priv->screensaver_proxy);

        play_loop_stop (&manager->priv->critical_alert_timeout_id);

        g_clear_object (&manager->priv->idle_monitor);

        if (manager->priv->xscreensaver_watchdog_timer_id > 0) {
                g_source_remove (manager->priv->xscreensaver_watchdog_timer_id);
                manager->priv->xscreensaver_watchdog_timer_id = 0;
        }
}

static void
gsd_power_manager_init (GsdPowerManager *manager)
{
        manager->priv = GSD_POWER_MANAGER_GET_PRIVATE (manager);
        manager->priv->inhibit_lid_switch_fd = -1;
        manager->priv->inhibit_suspend_fd = -1;
        manager->priv->bus_cancellable = g_cancellable_new ();
}

/* returns new level */
static void
handle_method_call_keyboard (GsdPowerManager *manager,
                             const gchar *method_name,
                             GVariant *parameters,
                             GDBusMethodInvocation *invocation)
{
        gint step;
        gint value = -1;
        gboolean ret;
        guint percentage;
        GError *error = NULL;

        if (g_strcmp0 (method_name, "StepUp") == 0) {
                g_debug ("keyboard step up");
                step = BRIGHTNESS_STEP_AMOUNT (manager->priv->kbd_brightness_max);
                value = MIN (manager->priv->kbd_brightness_now + step,
                             manager->priv->kbd_brightness_max);
                ret = upower_kbd_set_brightness (manager, value, &error);

        } else if (g_strcmp0 (method_name, "StepDown") == 0) {
                g_debug ("keyboard step down");
                step = BRIGHTNESS_STEP_AMOUNT (manager->priv->kbd_brightness_max);
                value = MAX (manager->priv->kbd_brightness_now - step, 0);
                ret = upower_kbd_set_brightness (manager, value, &error);

        } else if (g_strcmp0 (method_name, "Toggle") == 0) {
                value = upower_kbd_toggle (manager, &error);
                ret = (value >= 0);

        } else {
                g_assert_not_reached ();
        }

        /* return value */
        if (!ret) {
                g_dbus_method_invocation_take_error (invocation,
                                                     error);
                backlight_iface_emit_changed (manager, GSD_POWER_DBUS_INTERFACE_KEYBOARD, -1);
        } else {
                percentage = ABS_TO_PERCENTAGE (0,
                                                manager->priv->kbd_brightness_max,
                                                value);
                g_dbus_method_invocation_return_value (invocation,
                                                       g_variant_new ("(i)",
                                                                      percentage));
                backlight_iface_emit_changed (manager, GSD_POWER_DBUS_INTERFACE_KEYBOARD, percentage);
        }
}

static void
handle_method_call_screen (GsdPowerManager *manager,
                           const gchar *method_name,
                           GVariant *parameters,
                           GDBusMethodInvocation *invocation)
{
        gint value = -1;
        GError *error = NULL;

        if (!manager->priv->backlight_available) {
               g_set_error_literal (&error,
                                    GSD_POWER_MANAGER_ERROR,
                                    GSD_POWER_MANAGER_ERROR_FAILED,
                                    "Screen backlight not available");
                goto out;
        }

        if (g_strcmp0 (method_name, "StepUp") == 0) {
                g_debug ("screen step up");
                value = backlight_step_up (manager->priv->rr_screen, &error);
                backlight_iface_emit_changed (manager, GSD_POWER_DBUS_INTERFACE_SCREEN, value);
        } else if (g_strcmp0 (method_name, "StepDown") == 0) {
                g_debug ("screen step down");
                value = backlight_step_down (manager->priv->rr_screen, &error);
                backlight_iface_emit_changed (manager, GSD_POWER_DBUS_INTERFACE_SCREEN, value);
        } else {
                g_assert_not_reached ();
        }

out:
        /* return value */
        if (value < 0) {
                g_dbus_method_invocation_take_error (invocation,
                                                     error);
        } else {
                g_dbus_method_invocation_return_value (invocation,
                                                       g_variant_new ("(i)",
                                                                      value));
        }
}

static void
handle_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        /* Check session pointer as a proxy for whether the manager is in the
           start or stop state */
        if (manager->priv->session == NULL) {
                return;
        }

        g_debug ("Calling method '%s.%s' for Power",
                 interface_name, method_name);

        if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_SCREEN) == 0) {
                handle_method_call_screen (manager,
                                           method_name,
                                           parameters,
                                           invocation);
        } else if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_KEYBOARD) == 0) {
                handle_method_call_keyboard (manager,
                                             method_name,
                                             parameters,
                                             invocation);
        } else {
                g_warning ("not recognised interface: %s", interface_name);
        }
}

static GVariant *
handle_get_property_other (GsdPowerManager *manager,
                           const gchar *interface_name,
                           const gchar *property_name,
                           GError **error)
{
        GVariant *retval = NULL;
        gint32 value;

        if (g_strcmp0 (property_name, "Brightness") != 0) {
                g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                             "No such property: %s", property_name);
                return NULL;
        }

        if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_SCREEN) == 0) {
                value = backlight_get_percentage (manager->priv->rr_screen, NULL);
                retval = g_variant_new_int32 (value);
        } else if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_KEYBOARD) == 0) {
                value = manager->priv->kbd_brightness_now;
                retval =  g_variant_new_int32 (value);
        }

        if (retval == NULL) {
                g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                             "Failed to get property: %s", property_name);
        }
        return retval;
}

static GVariant *
handle_get_property (GDBusConnection *connection,
                     const gchar *sender,
                     const gchar *object_path,
                     const gchar *interface_name,
                     const gchar *property_name,
                     GError **error, gpointer user_data)
{
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        /* Check session pointer as a proxy for whether the manager is in the
           start or stop state */
        if (manager->priv->session == NULL) {
                g_set_error_literal (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                     "No session");
                return NULL;
        }

        if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_SCREEN) == 0 ||
                   g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_KEYBOARD) == 0) {
                return handle_get_property_other (manager, interface_name, property_name, error);
        } else {
                g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                             "No such interface: %s", interface_name);
                return NULL;
        }
}

static gboolean
handle_set_property_other (GsdPowerManager *manager,
                           const gchar *interface_name,
                           const gchar *property_name,
                           GVariant *value,
                           GError **error)
{
        gint32 brightness_value;

        if (g_strcmp0 (property_name, "Brightness") != 0) {
                g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                             "No such property: %s", property_name);
                return FALSE;
        }

        if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_SCREEN) == 0) {
                g_variant_get (value, "i", &brightness_value);
                return backlight_set_percentage (manager->priv->rr_screen,
                                                 brightness_value, error);
        } else if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_KEYBOARD) == 0) {
                g_variant_get (value, "i", &brightness_value);
                brightness_value = PERCENTAGE_TO_ABS (0, manager->priv->kbd_brightness_max,
                                                      brightness_value);
                return upower_kbd_set_brightness (manager, brightness_value, error);
        }

        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                     "No such interface: %s", interface_name);
        return FALSE;
}

static gboolean
handle_set_property (GDBusConnection *connection,
                     const gchar *sender,
                     const gchar *object_path,
                     const gchar *interface_name,
                     const gchar *property_name,
                     GVariant *value,
                     GError **error, gpointer user_data)
{
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        /* Check session pointer as a proxy for whether the manager is in the
           start or stop state */
        if (manager->priv->session == NULL) {
                return FALSE;
        }

        if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_SCREEN) == 0 ||
            g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_KEYBOARD) == 0) {
                return handle_set_property_other (manager, interface_name, property_name, value, error);
        } else {
                g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                             "No such interface: %s", interface_name);
                return FALSE;
        }
}

static const GDBusInterfaceVTable interface_vtable =
{
        handle_method_call,
        handle_get_property,
        handle_set_property
};

static void
on_bus_gotten (GObject             *source_object,
               GAsyncResult        *res,
               GsdPowerManager     *manager)
{
        GDBusConnection *connection;
        GDBusInterfaceInfo **infos;
        GError *error = NULL;
        guint i;

        connection = g_bus_get_finish (res, &error);
        if (connection == NULL) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Could not get session bus: %s", error->message);
                g_error_free (error);
                return;
        }

        manager->priv->connection = connection;

        infos = manager->priv->introspection_data->interfaces;
        for (i = 0; infos[i] != NULL; i++) {
                g_dbus_connection_register_object (connection,
                                                   GSD_POWER_DBUS_PATH,
                                                   infos[i],
                                                   &interface_vtable,
                                                   manager,
                                                   NULL,
                                                   NULL);
        }

        manager->priv->name_id = g_bus_own_name_on_connection (connection,
                                                               GSD_POWER_DBUS_NAME,
                                                               G_BUS_NAME_OWNER_FLAGS_NONE,
                                                               NULL,
                                                               NULL,
                                                               NULL,
                                                               NULL);
}

static void
register_manager_dbus (GsdPowerManager *manager)
{
        manager->priv->introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
        g_assert (manager->priv->introspection_data != NULL);

        g_bus_get (G_BUS_TYPE_SESSION,
                   manager->priv->bus_cancellable,
                   (GAsyncReadyCallback) on_bus_gotten,
                   manager);
}

GsdPowerManager *
gsd_power_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (GSD_TYPE_POWER_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
                register_manager_dbus (manager_object);
        }
        return GSD_POWER_MANAGER (manager_object);
}
