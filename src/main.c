/*
 * Copyright (C) 2011 Nick Schermer <nick@xfce.org>
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#include <gtk/gtk.h>
#include <libxfce4util/libxfce4util.h>
#include <libxfce4ui/libxfce4ui.h>
#include <garcon/garcon.h>
#include <xfconf/xfconf.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <src/appfinder-window.h>
#include <src/appfinder-private.h>
#include <src/appfinder-model.h>



#define APPFINDER_DBUS_SERVICE     "org.xfce.Appfinder"
#define APPFINDER_DBUS_INTERFACE   APPFINDER_DBUS_SERVICE
#define APPFINDER_DBUS_PATH        "/org/xfce/Appfinder"
#define APPFINDER_DBUS_METHOD_OPEN "OpenWindow"
#define APPFINDER_DBUS_METHOD_QUIT "Quit"
#define APPFINDER_DBUS_ERROR       APPFINDER_DBUS_SERVICE ".Error"



static gboolean            opt_collapsed = FALSE;
static gboolean            opt_version = FALSE;
static gboolean            opt_replace = FALSE;
static gboolean            opt_quit = FALSE;
static gboolean            opt_disable_server = FALSE;
static GSList             *windows = NULL;
static gboolean            service_owner = FALSE;
static XfceAppfinderModel *model_cache = NULL;

#ifdef DEBUG
static GHashTable         *objects_table = NULL;
static guint               objects_table_count = 0;
#endif



static GOptionEntry option_entries[] =
{
  { "collapsed", 'c', 0, G_OPTION_ARG_NONE, &opt_collapsed, N_("Start in collapsed mode"), NULL },
  { "version", 'V', 0, G_OPTION_ARG_NONE, &opt_version, N_("Print version information and exit"), NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, N_("Replace the existing service"), NULL },
  { "quit", 'q', 0, G_OPTION_ARG_NONE, &opt_quit, N_("Quit all instances"), NULL },
  { "disable-server", 0, 0, G_OPTION_ARG_NONE, &opt_disable_server, N_("Do not try to use or become a D-Bus service"), NULL },
  { NULL }
};



static void appfinder_dbus_unregister (DBusConnection *dbus_connection);



#ifdef DEBUG
static void
appfinder_refcount_debug_weak_notify (gpointer  data,
                                      GObject  *where_the_object_was)
{
  /* remove the unreffed object pixbuf from the table */
  if (!g_hash_table_remove (objects_table, where_the_object_was))
    appfinder_assert_not_reached ();
}



static void
appfinder_refcount_debug_print (gpointer object,
                                gpointer desc,
                                gpointer data)
{
  APPFINDER_DEBUG ("object %p (type = %s, desc = %s) not released",
                   object, G_OBJECT_TYPE_NAME (object), (gchar *) desc);
}



void
appfinder_refcount_debug_add (GObject     *object,
                              const gchar *description)
{
  /* silently ignore objects that are already registered */
  if (object != NULL
      && g_hash_table_lookup (objects_table, object) == NULL)
    {
      objects_table_count++;
      g_object_weak_ref (G_OBJECT (object), appfinder_refcount_debug_weak_notify, NULL);
      g_hash_table_insert (objects_table, object, g_strdup (description));
    }
}



static void
appfinder_refcount_debug_init (void)
{
  objects_table = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
}



static void
appfinder_refcount_debug_finalize (void)
{
  /* leakup refcount hashtable */
  APPFINDER_DEBUG ("%d objects leaked, %d registered",
                   g_hash_table_size (objects_table),
                   objects_table_count);

  g_hash_table_foreach (objects_table, appfinder_refcount_debug_print, NULL);
  g_hash_table_destroy (objects_table);
}
#endif



static void
appfinder_window_destroyed (GtkWidget *window)
{
  XfconfChannel *channel;

  if (windows == NULL)
    return;

  /* take a reference on the model */
  if (model_cache == NULL)
    {
      APPFINDER_DEBUG ("main took reference on the main model");
      model_cache = xfce_appfinder_model_get ();
    }

  /* remove from internal list */
  windows = g_slist_remove (windows, window);

  /* check if we're going to the background
   * if the last window is closed */
  if (windows == NULL)
    {
      if (!service_owner)
        {
          /* leave if we're not the daemon or started
           * with --disable-server */
          gtk_main_quit ();
        }
      else
        {
          /* leave if the user disable the service in the prefereces */
          channel = xfconf_channel_get ("xfce4-appfinder");
          if (!xfconf_channel_get_bool (channel, "/enable-service", TRUE))
            gtk_main_quit ();
        }
    }
}



static void
appfinder_window_new (const gchar *startup_id,
                      gboolean     expanded)
{
  GtkWidget *window;

  window = g_object_new (XFCE_TYPE_APPFINDER_WINDOW,
                         "startup-id", IS_STRING (startup_id) ? startup_id : NULL,
                         NULL);
  appfinder_refcount_debug_add (G_OBJECT (window), startup_id);
  xfce_appfinder_window_set_expanded (XFCE_APPFINDER_WINDOW (window), expanded);
  gtk_widget_show (window);

  windows = g_slist_prepend (windows, window);
  g_signal_connect (G_OBJECT (window), "destroy",
                    G_CALLBACK (appfinder_window_destroyed), NULL);
}



static DBusHandlerResult
appfinder_dbus_message (DBusConnection *dbus_connection,
                        DBusMessage    *message,
                        gpointer        user_data)
{
  DBusMessage *reply;
  gboolean     expanded;
  gchar       *startup_id;
  DBusError    derror;

  if (dbus_message_is_method_call (message, APPFINDER_DBUS_INTERFACE, APPFINDER_DBUS_METHOD_OPEN))
    {
      dbus_error_init (&derror);
      if (dbus_message_get_args (message, &derror,
                                 DBUS_TYPE_BOOLEAN, &expanded,
                                 DBUS_TYPE_STRING, &startup_id,
                                 DBUS_TYPE_INVALID))
        {
          appfinder_window_new (startup_id, expanded);
          reply = dbus_message_new_method_return (message);
        }
      else
        {
          reply = dbus_message_new_error (message, APPFINDER_DBUS_ERROR, derror.message);
          dbus_error_free (&derror);
        }

      dbus_connection_send (dbus_connection, reply, NULL);
      dbus_message_unref (reply);
    }
  else if (dbus_message_is_method_call (message, APPFINDER_DBUS_INTERFACE, APPFINDER_DBUS_METHOD_QUIT))
    {
      /* close all windows and quit */
      g_printerr ("%s: %s.\n", PACKAGE_NAME, _("Forced to quit"));
      gtk_main_quit ();
    }
  else if (dbus_message_is_signal (message, DBUS_INTERFACE_LOCAL, "Disconnected")
           || dbus_message_is_signal (message, DBUS_INTERFACE_DBUS, "NameOwnerChanged"))
    {
      if (windows != NULL)
        {
          /* don't respond to dbus signals and close on last window */
          appfinder_dbus_unregister (dbus_connection);
        }
      else
        {
          /* no active windows, just exit the instance */
          gtk_main_quit ();
        }
    }
  else
    {
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

  return DBUS_HANDLER_RESULT_HANDLED;
}



static gboolean
appfinder_dbus_open_window (DBusConnection *dbus_connection,
                            const gchar    *startup_id)
{

  DBusError    derror;
  DBusMessage *method, *result;
  gboolean     expanded = !opt_collapsed;

  method = dbus_message_new_method_call (APPFINDER_DBUS_SERVICE,
                                         APPFINDER_DBUS_PATH,
                                         APPFINDER_DBUS_INTERFACE,
                                         APPFINDER_DBUS_METHOD_OPEN);

  if (startup_id == NULL)
    startup_id = "";

  dbus_message_append_args (method,
                            DBUS_TYPE_BOOLEAN, &expanded,
                            DBUS_TYPE_STRING, &startup_id,
                            DBUS_TYPE_INVALID);

  dbus_message_set_auto_start (method, TRUE);
  dbus_error_init (&derror);
  result = dbus_connection_send_with_reply_and_block (dbus_connection, method, 5000, &derror);
  dbus_message_unref (method);

  if (G_UNLIKELY (result == NULL))
    {
       g_critical ("Failed to open window: %s", derror.message);
       dbus_error_free(&derror);
       return FALSE;
    }

  dbus_message_unref (result);

  return TRUE;
}



static gint
appfinder_dbus_quit (void)
{
  DBusMessage    *method;
  DBusConnection *dbus_connection;
  DBusError       derror;
  gboolean        succeed = FALSE;

  dbus_error_init (&derror);
  dbus_connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (G_LIKELY (dbus_connection != NULL))
    {
      method = dbus_message_new_method_call (APPFINDER_DBUS_SERVICE,
                                             APPFINDER_DBUS_PATH,
                                             APPFINDER_DBUS_INTERFACE,
                                             APPFINDER_DBUS_METHOD_QUIT);

      dbus_message_set_auto_start (method, FALSE);
      succeed = dbus_connection_send (dbus_connection, method, NULL);
      dbus_message_unref (method);

      dbus_connection_flush (dbus_connection);
      dbus_connection_unref (dbus_connection);
    }
  else
    {
      g_warning ("Unable to open D-Bus connection: %s", derror.message);
      dbus_error_free (&derror);
    }

  return succeed ? EXIT_SUCCESS : EXIT_FAILURE;
}



static void
appfinder_dbus_unregister (DBusConnection *dbus_connection)
{
  if (service_owner)
    {
      service_owner = FALSE;

      dbus_connection_remove_filter (dbus_connection, appfinder_dbus_message, NULL);
      dbus_connection_unregister_object_path (dbus_connection, APPFINDER_DBUS_PATH);
      dbus_bus_release_name (dbus_connection, APPFINDER_DBUS_SERVICE, NULL);
    }
}



static gint
appfinder_daemonize (void)
{
#ifdef HAVE_DAEMON
  return daemon (1, 1);
#else
  pid_t pid;

  pid = fork ();
  if (pid < 0)
    return -1;

  if (pid > 0)
    _exit (EXIT_SUCCESS);

#ifdef HAVE_SETSID
  if (setsid () < 0)
    return -1;
#endif

  return 0;
#endif
}



static void
appfinder_signal_handler (gint signum)
{
  static gboolean was_triggered = FALSE;

  /* avoid recursing this handler */
  if (was_triggered)
    return;
  was_triggered = TRUE;

  APPFINDER_DEBUG ("received signal %s", g_strsignal (signum));

  gtk_main_quit ();
}



static DBusConnection *
appfinder_dbus_service (const gchar *startup_id)
{
  DBusError             derror;
  DBusConnection       *dbus_connection;
  guint                 dbus_flags;
  DBusObjectPathVTable  vtable = { NULL, appfinder_dbus_message, NULL, };
  gint                  result;

  /* become the serivce owner or ask the current owner to spawn an instance */
  dbus_error_init (&derror);
  dbus_connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (G_LIKELY (dbus_connection != NULL))
    {
      dbus_connection_set_exit_on_disconnect (dbus_connection, FALSE);

      dbus_flags = DBUS_NAME_FLAG_DO_NOT_QUEUE | DBUS_NAME_FLAG_ALLOW_REPLACEMENT;
      if (opt_replace)
        dbus_flags |= DBUS_NAME_FLAG_REPLACE_EXISTING;

      result = dbus_bus_request_name (dbus_connection, APPFINDER_DBUS_SERVICE, dbus_flags, &derror);
      if (result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
        {
          dbus_connection_setup_with_g_main (dbus_connection, NULL);

          /* watch owner changes */
          dbus_bus_add_match (dbus_connection, "type='signal',member='NameOwnerChanged',"
                                               "arg0='"APPFINDER_DBUS_SERVICE"'", NULL);

          /* method handling for the appfinder */
          if (dbus_connection_register_object_path (dbus_connection, APPFINDER_DBUS_PATH, &vtable, NULL)
              && dbus_connection_add_filter (dbus_connection, appfinder_dbus_message, NULL, NULL))
            {
              APPFINDER_DEBUG ("registered dbus service");

              /* successfully registered the service */
              service_owner = TRUE;

              /* fork to the background */
              if (appfinder_daemonize () == -1)
                {
                  xfce_message_dialog (NULL, _("Application Finder"),
                                       GTK_STOCK_DIALOG_ERROR,
                                       _("Unable to daemonize the process"),
                                       g_strerror (errno),
                                       GTK_STOCK_QUIT, GTK_RESPONSE_ACCEPT,
                                       NULL);

                  _exit (EXIT_FAILURE);
                }

              APPFINDER_DEBUG ("daemonized the process");

            }
          else
            {
              g_warning ("Failed to register D-Bus filter or vtable");
            }
        }
      else if (result == DBUS_REQUEST_NAME_REPLY_EXISTS)
        {
          if (appfinder_dbus_open_window (dbus_connection, startup_id))
            {
               /* successfully opened a window in the other instance */
               dbus_connection_unref (dbus_connection);
               _exit (EXIT_SUCCESS);
            }
        }
      else
        {
          g_warning ("Unable to request D-Bus name: %s", derror.message);
          dbus_error_free (&derror);

          dbus_connection_unref (dbus_connection);
          dbus_connection = NULL;
        }
    }
  else
    {
      g_warning ("Unable to open D-Bus connection: %s", derror.message);
      dbus_error_free (&derror);
    }

  return dbus_connection;
}



gint
main (gint argc, gchar **argv)
{
  GError         *error = NULL;
  const gchar    *desktop;
  DBusConnection *dbus_connection = NULL;
  const gchar    *startup_id;
  GSList         *windows_destroy;
  const gint      signums[] = { SIGINT, SIGQUIT, SIGTERM, SIGABRT };
  guint           i;

  /* set translation domain */
  xfce_textdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR, "UTF-8");

#ifdef G_ENABLE_DEBUG
  /* do NOT remove this line for now, If something doesn't work,
   * fix your code instead! */
  g_log_set_always_fatal (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING);
#endif

#if !GLIB_CHECK_VERSION (2, 32, 0)
  if (!g_thread_supported ())
    g_thread_init (NULL);
#endif

  /* get the startup notification id */
  startup_id = g_getenv ("DESKTOP_STARTUP_ID");

  if (!gtk_init_with_args (&argc, &argv, NULL, option_entries, GETTEXT_PACKAGE, &error))
    {
      g_printerr ("%s: %s.\n", PACKAGE_NAME, error->message);
      g_printerr (_("Type \"%s --help\" for usage."), PACKAGE_NAME);
      g_printerr ("\n");
      g_error_free (error);

      return EXIT_FAILURE;
    }

  if (opt_version)
    {
      g_print ("%s %s (Xfce %s)\n\n", PACKAGE_NAME, PACKAGE_VERSION, xfce_version_string ());
      g_print ("%s\n", "Copyright (c) 2004-2011");
      g_print ("\t%s\n\n", _("The Xfce development team. All rights reserved."));
      g_print (_("Please report bugs to <%s>."), PACKAGE_BUGREPORT);
      g_print ("\n");

      return EXIT_SUCCESS;
    }

  if (opt_quit)
    return appfinder_dbus_quit ();

  /* setup signal handlers to properly quit the main loop */
  for (i = 0; i < G_N_ELEMENTS (signums); i++)
    signal (signums[i], appfinder_signal_handler);

  /* if started with the xfrun4 executable, start in collapsed mode */
  if (!opt_collapsed && strcmp (*argv, "xfrun4") == 0)
    opt_collapsed = TRUE;

  /* become the serivce owner or ask the current
   * owner to spawn an instance */
  if (G_LIKELY (!opt_disable_server))
    dbus_connection = appfinder_dbus_service (startup_id);

  /* if the value is unset, fallback to XFCE, if the
   * value is empty, allow all applications in the menu */
  desktop = g_getenv ("XDG_CURRENT_DESKTOP");
  if (G_LIKELY (desktop == NULL))
    desktop = "XFCE";
  else if (*desktop == '\0')
    desktop = NULL;
  garcon_set_environment (desktop);

  if (!xfconf_init (&error))
    {
       g_critical ("Failed to initialized xfconf: %s", error->message);
       g_error_free (error);

       return EXIT_FAILURE;
    }

#ifdef DEBUG
  appfinder_refcount_debug_init ();
#endif

  /* create initial window */
  appfinder_window_new (NULL, !opt_collapsed);

  APPFINDER_DEBUG ("enter mainloop");

  gtk_main ();

  /* release the model cache */
  if (model_cache != NULL)
    g_object_unref (G_OBJECT (model_cache));

  if (G_LIKELY (dbus_connection != NULL))
    {
      appfinder_dbus_unregister (dbus_connection);
      dbus_connection_unref (dbus_connection);
    }

  if (windows != NULL)
    {
      /* avoid calling appfinder_window_destroyed */
      windows_destroy = windows;
      windows = NULL;

      /* destroy all windows */
      g_slist_foreach (windows_destroy, (GFunc) gtk_widget_destroy, NULL);
      g_slist_free (windows_destroy);
    }

  xfconf_shutdown ();

#ifdef DEBUG
  appfinder_refcount_debug_finalize ();
#endif

  return EXIT_SUCCESS;
}
