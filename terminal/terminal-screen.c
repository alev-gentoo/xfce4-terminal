/* $Id: terminal-screen.c 57 2004-11-22 17:38:15Z bmeurer $ */
/*-
 * Copyright (c) 2004 os-cillation e.K.
 *
 * Written by Benedikt Meurer <benny@xfce.org>.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#include <stdlib.h>
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <exo/exo.h>

#include <terminal/terminal-image-loader.h>
#include <terminal/terminal-screen.h>
#include <terminal/terminal-widget.h>



enum
{
  PROP_0,
  PROP_CUSTOM_TITLE,
  PROP_TITLE,
};

enum
{
  CONTEXT_MENU,
  SELECTION_CHANGED,
  LAST_SIGNAL,
};



struct _TerminalScreenClass
{
  GtkHBoxClass __parent__;

  /* signals */
  void (*context_menu)      (TerminalScreen *screen, GdkEvent *event);
  void (*selection_changed) (TerminalScreen *screen);
};

struct _TerminalScreen
{
  GtkHBox              __parent__;
  TerminalPreferences *preferences;
  GtkWidget           *terminal;
  GtkWidget           *scrollbar;

  GPid                 pid;
  gchar               *working_directory;

  gchar              **custom_command;
  gchar               *custom_title;

  guint                background_timer_id;
};



static void     terminal_screen_finalize                      (GObject          *object);
static void     terminal_screen_get_property                  (GObject          *object,
                                                               guint             prop_id,
                                                               GValue           *value,
                                                               GParamSpec       *pspec);
static void     terminal_screen_set_property                  (GObject          *object,
                                                               guint             prop_id,
                                                               const GValue     *value,
                                                               GParamSpec       *pspec);
static gboolean terminal_screen_get_child_command             (TerminalScreen   *screen,
                                                               gchar           **command,
                                                               gchar          ***argv,
                                                               GError          **error);
static gchar  **terminal_screen_get_child_environment         (TerminalScreen   *screen);
static void terminal_screen_update_background                 (TerminalScreen   *screen);
static void terminal_screen_update_binding_backspace          (TerminalScreen   *screen);
static void terminal_screen_update_binding_delete             (TerminalScreen   *screen);
static void terminal_screen_update_colors                     (TerminalScreen   *screen);
static void terminal_screen_update_font                       (TerminalScreen   *screen);
static void terminal_screen_update_misc_bell                  (TerminalScreen   *screen);
static void terminal_screen_update_misc_cursor_blinks         (TerminalScreen   *screen);
static void terminal_screen_update_scrolling_bar              (TerminalScreen   *screen);
static void terminal_screen_update_scrolling_lines            (TerminalScreen   *screen);
static void terminal_screen_update_scrolling_on_output        (TerminalScreen   *screen);
static void terminal_screen_update_scrolling_on_keystroke     (TerminalScreen   *screen);
static void terminal_screen_update_title                      (TerminalScreen   *screen);
static void terminal_screen_update_word_chars                 (TerminalScreen   *screen);
static void terminal_screen_vte_child_exited                  (VteTerminal      *terminal,
                                                               TerminalScreen   *screen);
static void     terminal_screen_vte_eof                       (VteTerminal    *terminal,
                                                               TerminalScreen *screen);
static void     terminal_screen_vte_context_menu              (TerminalWidget *widget,
                                                               GdkEvent       *event,
                                                               TerminalScreen *screen);
static void     terminal_screen_vte_realize                   (VteTerminal    *terminal,
                                                               TerminalScreen *screen);
static void     terminal_screen_vte_map                       (VteTerminal    *terminal,
                                                               TerminalScreen *screen);
static void     terminal_screen_vte_selection_changed         (VteTerminal    *terminal,
                                                               TerminalScreen *screen);
static void     terminal_screen_vte_window_title_changed      (VteTerminal    *terminal,
                                                               TerminalScreen *screen);
static gboolean terminal_screen_timer_background              (gpointer        user_data);
static void     terminal_screen_timer_background_destroy      (gpointer        user_data);



static GObjectClass *parent_class;
static guint screen_signals[LAST_SIGNAL];



G_DEFINE_TYPE (TerminalScreen, terminal_screen, GTK_TYPE_HBOX);



static void
terminal_screen_class_init (TerminalScreenClass *klass)
{
  GObjectClass *gobject_class;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = terminal_screen_finalize;
  gobject_class->get_property = terminal_screen_get_property;
  gobject_class->set_property = terminal_screen_set_property;

  /**
   * TerminalScreen:custom-title:
   **/
  g_object_class_install_property (gobject_class,
                                   PROP_CUSTOM_TITLE,
                                   g_param_spec_string ("custom-title",
                                                        _("Custom title"),
                                                        _("Custom title"),
                                                        NULL,
                                                        G_PARAM_READWRITE));

  /**
   * TerminalScreen:title:
   **/
  g_object_class_install_property (gobject_class,
                                   PROP_TITLE,
                                   g_param_spec_string ("title",
                                                        _("Title"),
                                                        _("Title"),
                                                        NULL,
                                                        G_PARAM_READABLE));

  /**
   * TerminalScreen::context-menu
   **/
  screen_signals[CONTEXT_MENU] =
    g_signal_new ("context-menu",
                  G_TYPE_FROM_CLASS (gobject_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, context_menu),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__POINTER,
                  G_TYPE_NONE, 1, G_TYPE_POINTER);

  /**
   * TerminalScreen::selection-changed
   **/
  screen_signals[SELECTION_CHANGED] =
    g_signal_new ("selection-changed",
                  G_TYPE_FROM_CLASS (gobject_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, selection_changed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
}



static void
terminal_screen_init (TerminalScreen *screen)
{
  screen->working_directory = g_get_current_dir ();
  screen->custom_title = g_strdup ("");

  screen->terminal = terminal_widget_new ();
  g_object_connect (G_OBJECT (screen->terminal),
                    "signal::child-exited", G_CALLBACK (terminal_screen_vte_child_exited), screen,
                    "signal::eof", G_CALLBACK (terminal_screen_vte_eof), screen,
                    "signal::context-menu", G_CALLBACK (terminal_screen_vte_context_menu), screen,
                    "signal::selection-changed", G_CALLBACK (terminal_screen_vte_selection_changed), screen,
                    "signal::window-title-changed", G_CALLBACK (terminal_screen_vte_window_title_changed), screen,
                    "signal-after::realize", G_CALLBACK (terminal_screen_vte_realize), screen,
                    "signal-after::map", G_CALLBACK (terminal_screen_vte_map), screen,
                    "swapped-signal::size-allocate", G_CALLBACK (terminal_screen_timer_background), screen,
                    "swapped-signal::style-set", G_CALLBACK (terminal_screen_update_colors), screen,
                    NULL);
  gtk_box_pack_start (GTK_BOX (screen), screen->terminal, TRUE, TRUE, 0);
  gtk_widget_show (screen->terminal);

  screen->scrollbar = gtk_vscrollbar_new (VTE_TERMINAL (screen->terminal)->adjustment);
  gtk_box_pack_start (GTK_BOX (screen), screen->scrollbar, FALSE, FALSE, 0);
  gtk_widget_show (screen->scrollbar);

  screen->preferences = terminal_preferences_get ();
  g_object_connect (G_OBJECT (screen->preferences),
                    "swapped-signal::notify::background-mode", G_CALLBACK (terminal_screen_update_background), screen,
                    "swapped-signal::notify::background-image-file", G_CALLBACK (terminal_screen_update_background), screen,
                    "swapped-signal::notify::background-image-style", G_CALLBACK (terminal_screen_update_background), screen,
                    "swapped-signal::notify::background-darkness", G_CALLBACK (terminal_screen_update_background), screen,
                    "swapped-signal::notify::binding-backspace", G_CALLBACK (terminal_screen_update_binding_backspace), screen,
                    "swapped-signal::notify::binding-delete", G_CALLBACK (terminal_screen_update_binding_delete), screen,
                    "swapped-signal::notify::color-system-theme", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-foreground", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-background", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette1", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette2", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette3", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette4", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette5", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette6", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette7", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette8", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette9", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette10", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette11", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette12", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette13", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette14", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette15", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::color-palette16", G_CALLBACK (terminal_screen_update_colors), screen,
                    "swapped-signal::notify::font-anti-alias", G_CALLBACK (terminal_screen_update_font), screen,
                    "swapped-signal::notify::font-name", G_CALLBACK (terminal_screen_update_font), screen,
                    "swapped-signal::notify::misc-bell", G_CALLBACK (terminal_screen_update_misc_bell), screen,
                    "swapped-signal::notify::misc-cursor-blinks", G_CALLBACK (terminal_screen_update_misc_cursor_blinks), screen,
                    "swapped-signal::notify::scrolling-bar", G_CALLBACK (terminal_screen_update_scrolling_bar), screen,
                    "swapped-signal::notify::scrolling-lines", G_CALLBACK (terminal_screen_update_scrolling_lines), screen,
                    "swapped-signal::notify::scrolling-on-output", G_CALLBACK (terminal_screen_update_scrolling_on_output), screen,
                    "swapped-signal::notify::scrolling-on-keystroke", G_CALLBACK (terminal_screen_update_scrolling_on_keystroke), screen,
                    "swapped-signal::notify::title-initial", G_CALLBACK (terminal_screen_update_title), screen,
                    "swapped-signal::notify::title-mode", G_CALLBACK (terminal_screen_update_title), screen,
                    "swapped-signal::notify::word-chars", G_CALLBACK (terminal_screen_update_word_chars), screen,
                    NULL);

  /* apply current settings */
  terminal_screen_update_binding_backspace (screen);
  terminal_screen_update_binding_delete (screen);
  terminal_screen_update_font (screen);
  terminal_screen_update_misc_bell (screen);
  terminal_screen_update_misc_cursor_blinks (screen);
  terminal_screen_update_scrolling_bar (screen);
  terminal_screen_update_scrolling_lines (screen);
  terminal_screen_update_scrolling_on_output (screen);
  terminal_screen_update_scrolling_on_keystroke (screen);
  terminal_screen_update_word_chars (screen);
}



static void
terminal_screen_finalize (GObject *object)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);

  if (G_UNLIKELY (screen->background_timer_id != 0))
    g_source_remove (screen->background_timer_id);

  g_signal_handlers_disconnect_matched (G_OBJECT (screen->preferences),
                                        G_SIGNAL_MATCH_DATA,
                                        0, 0, NULL, NULL, screen);

  g_object_unref (G_OBJECT (screen->preferences));

  g_free (screen->working_directory);
  g_strfreev (screen->custom_command);
  g_free (screen->custom_title);

  parent_class->finalize (object);
}



static void
terminal_screen_get_property (GObject          *object,
                              guint             prop_id,
                              GValue           *value,
                              GParamSpec       *pspec)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);

  switch (prop_id)
    {
    case PROP_CUSTOM_TITLE:
      g_value_set_string (value, screen->custom_title);
      break;

    case PROP_TITLE:
      g_value_take_string (value, terminal_screen_get_title (screen));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static void
terminal_screen_set_property (GObject          *object,
                              guint             prop_id,
                              const GValue     *value,
                              GParamSpec       *pspec)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);

  switch (prop_id)
    {
    case PROP_CUSTOM_TITLE:
      terminal_screen_set_custom_title (screen, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static gboolean
terminal_screen_get_child_command (TerminalScreen   *screen,
                                   gchar           **command,
                                   gchar          ***argv,
                                   GError          **error)
{
  struct passwd *pw;
  const gchar   *shell_name;
  gboolean       command_login_shell;

  if (screen->custom_command != NULL)
    {
      *command = g_strdup (screen->custom_command[0]);
      *argv    = g_strdupv (screen->custom_command);
    }
  else
    {
      pw = getpwuid (getuid ());
      if (G_UNLIKELY (pw == NULL))
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                       _("Current user unknown"));
          return FALSE;
        }

      g_object_get (G_OBJECT (screen->preferences),
                    "command-login-shell", &command_login_shell,
                    NULL);

      shell_name = strrchr (pw->pw_shell, '/');
      if (shell_name != NULL)
        ++shell_name;
      else
        shell_name = pw->pw_shell;
      *command = g_strdup (pw->pw_shell);

      *argv = g_new (gchar *, 2);
      if (command_login_shell)
        (*argv)[0] = g_strconcat ("-", shell_name, NULL);
      else
        (*argv)[0] = g_strdup (shell_name);
      (*argv)[1] = NULL;
    }

  return TRUE;
}



static gchar**
terminal_screen_get_child_environment (TerminalScreen *screen)
{
  extern gchar    **environ;
  gchar           **result;
  gchar           **p;
  gchar            *term;
  guint             n;

  /* count env vars that are set */
  for (p = environ; *p != NULL; ++p);

  n = p - environ;
  result = g_new (gchar *, n + 1 + 4);

  for (n = 0, p = environ; *p != NULL; ++p)
    {
      if ((strncmp (*p, "COLUMNS=", 8) == 0)
          || (strncmp (*p, "LINES=", 6) == 0)
          || (strncmp (*p, "WINDOWID=", 9) == 0)
          || (strncmp (*p, "TERM=", 5) == 0)
          || (strncmp (*p, "GNOME_DESKTOP_ICON=", 19) == 0)
          || (strncmp (*p, "COLORTERM=", 10) == 0)
          || (strncmp ( *p, "DISPLAY=", 8) == 0))
        {
          /* nothing: do not copy */
        }
      else
        {
          result[n] = g_strdup (*p);
          ++n;
        }
    }

  result[n++] = g_strdup ("COLORTERM=Terminal");

  g_object_get (G_OBJECT (screen->preferences), "term", &term, NULL);
  if (G_LIKELY (term != NULL))
    {
      result[n++] = g_strdup_printf ("TERM=%s", term);
      g_free (term);
    }

  if (GTK_WIDGET_REALIZED (screen->terminal))
    {
      result[n++] = g_strdup_printf ("WINDOWID=%ld", (glong) GDK_WINDOW_XWINDOW (screen->terminal->window));
      result[n++] = g_strdup_printf ("DISPLAY=%s", gdk_display_get_name (gtk_widget_get_display (screen->terminal)));
    }

  result[n] = NULL;

  return result;
}



static void
terminal_screen_update_background (TerminalScreen *screen)
{
  if (G_UNLIKELY (screen->background_timer_id != 0))
    g_source_remove (screen->background_timer_id);

  screen->background_timer_id = g_timeout_add_full (G_PRIORITY_LOW, 250, terminal_screen_timer_background,
                                                    screen, terminal_screen_timer_background_destroy);
}



static void
terminal_screen_update_binding_backspace (TerminalScreen *screen)
{
  TerminalEraseBinding binding;

  g_object_get (G_OBJECT (screen->preferences), "binding-backspace", &binding, NULL);

  switch (binding)
    {
    case TERMINAL_ERASE_BINDING_ASCII_BACKSPACE:
      vte_terminal_set_backspace_binding (VTE_TERMINAL (screen->terminal), VTE_ERASE_ASCII_BACKSPACE);
      break;

    case TERMINAL_ERASE_BINDING_ASCII_DELETE:
      vte_terminal_set_backspace_binding (VTE_TERMINAL (screen->terminal), VTE_ERASE_ASCII_DELETE);
      break;

    case TERMINAL_ERASE_BINDING_DELETE_SEQUENCE:
      vte_terminal_set_backspace_binding (VTE_TERMINAL (screen->terminal), VTE_ERASE_DELETE_SEQUENCE);
      break;

    default:
      g_assert_not_reached ();
    }
}



static void
terminal_screen_update_binding_delete (TerminalScreen *screen)
{
  TerminalEraseBinding binding;

  g_object_get (G_OBJECT (screen->preferences), "binding-delete", &binding, NULL);

  switch (binding)
    {
    case TERMINAL_ERASE_BINDING_ASCII_BACKSPACE:
      vte_terminal_set_delete_binding (VTE_TERMINAL (screen->terminal), VTE_ERASE_ASCII_BACKSPACE);
      break;

    case TERMINAL_ERASE_BINDING_ASCII_DELETE:
      vte_terminal_set_delete_binding (VTE_TERMINAL (screen->terminal), VTE_ERASE_ASCII_DELETE);
      break;

    case TERMINAL_ERASE_BINDING_DELETE_SEQUENCE:
      vte_terminal_set_delete_binding (VTE_TERMINAL (screen->terminal), VTE_ERASE_DELETE_SEQUENCE);
      break;

    default:
      g_assert_not_reached ();
    }
}



static void
query_color (TerminalPreferences *preferences,
             const gchar         *property,
             GdkColor            *color_return)
{
  GdkColor *color;

  g_object_get (G_OBJECT (preferences), property, &color, NULL);
  *color_return = *color;
  gdk_color_free (color);
}



static void
terminal_screen_update_colors (TerminalScreen *screen)
{
  gboolean system_theme;
  GdkColor palette[16];
  GdkColor bg;
  GdkColor fg;
  gchar    name[32];
  guint    n;

  g_object_get (G_OBJECT (screen->preferences),
                "color-system-theme", &system_theme,
                NULL);

  if (system_theme)
    {
      bg = screen->terminal->style->base[GTK_STATE_NORMAL];
      fg = screen->terminal->style->text[GTK_STATE_NORMAL];
    }
  else
    {
      query_color (screen->preferences, "color-background", &bg);
      query_color (screen->preferences, "color-foreground", &fg);
    }

  for (n = 0; n < 16; ++n)
    {
      g_snprintf (name, 32, "color-palette%u", n + 1);
      query_color (screen->preferences, name, palette + n);
    }

  vte_terminal_set_colors (VTE_TERMINAL (screen->terminal), &fg, &bg, palette, 16);
  vte_terminal_set_background_tint_color (VTE_TERMINAL (screen->terminal), &bg);
}



static void
terminal_screen_update_font (TerminalScreen *screen)
{
  VteTerminalAntiAlias antialias;
  gboolean             font_anti_alias;
  gchar               *font_name;

  g_object_get (G_OBJECT (screen->preferences),
                "font-anti-alias", &font_anti_alias,
                "font-name", &font_name,
                NULL);

  if (G_LIKELY (font_name != NULL))
    {
      antialias = font_anti_alias
                ? VTE_ANTI_ALIAS_USE_DEFAULT
                : VTE_ANTI_ALIAS_FORCE_DISABLE;

      vte_terminal_set_font_from_string_full (VTE_TERMINAL (screen->terminal),
                                              font_name, antialias);
      g_free (font_name);
    }
}



static void
terminal_screen_update_misc_bell (TerminalScreen *screen)
{
  gboolean bval;
  g_object_get (G_OBJECT (screen->preferences), "misc-bell", &bval, NULL);
  vte_terminal_set_audible_bell (VTE_TERMINAL (screen->terminal), bval);
}



static void
terminal_screen_update_misc_cursor_blinks (TerminalScreen *screen)
{
  gboolean bval;
  g_object_get (G_OBJECT (screen->preferences), "misc-cursor-blinks", &bval, NULL);
  vte_terminal_set_cursor_blinks (VTE_TERMINAL (screen->terminal), bval);
}



static void
terminal_screen_update_scrolling_bar (TerminalScreen *screen)
{
  TerminalScrollbar scrollbar;

  g_object_get (G_OBJECT (screen->preferences), "scrolling-bar", &scrollbar, NULL);

  switch (scrollbar)
    {
    case TERMINAL_SCROLLBAR_NONE:
      gtk_widget_hide (screen->scrollbar);
      break;

    case TERMINAL_SCROLLBAR_LEFT:
      gtk_box_reorder_child (GTK_BOX (screen), screen->scrollbar, 0);
      gtk_widget_show (screen->scrollbar);
      break;

    case TERMINAL_SCROLLBAR_RIGHT:
      gtk_box_reorder_child (GTK_BOX (screen), screen->scrollbar, 1);
      gtk_widget_show (screen->scrollbar);
      break;
    }
}



static void
terminal_screen_update_scrolling_lines (TerminalScreen *screen)
{
  guint lines;
  g_object_get (G_OBJECT (screen->preferences), "scrolling-lines", &lines, NULL);
  vte_terminal_set_scrollback_lines (VTE_TERMINAL (screen->terminal), lines);
}



static void
terminal_screen_update_scrolling_on_output (TerminalScreen *screen)
{
  gboolean scroll;
  g_object_get (G_OBJECT (screen->preferences), "scrolling-on-output", &scroll, NULL);
  vte_terminal_set_scroll_on_output (VTE_TERMINAL (screen->terminal), scroll);
}



static void
terminal_screen_update_scrolling_on_keystroke (TerminalScreen *screen)
{
  gboolean scroll;
  g_object_get (G_OBJECT (screen->preferences), "scrolling-on-keystroke", &scroll, NULL);
  vte_terminal_set_scroll_on_keystroke (VTE_TERMINAL (screen->terminal), scroll);
}



static void
terminal_screen_update_title (TerminalScreen *screen)
{
  g_object_notify (G_OBJECT (screen), "title");
}



static void
terminal_screen_update_word_chars (TerminalScreen *screen)
{
  gchar *word_chars;

  g_object_get (G_OBJECT (screen->preferences), "word-chars", &word_chars, NULL);
  if (G_LIKELY (word_chars != NULL))
    {
      vte_terminal_set_word_chars (VTE_TERMINAL (screen->terminal), word_chars);
      g_free (word_chars);
    }
}



static void
terminal_screen_vte_child_exited (VteTerminal    *terminal,
                                  TerminalScreen *screen)
{
  g_return_if_fail (VTE_IS_TERMINAL (terminal));
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  gtk_widget_destroy (GTK_WIDGET (screen));
}



static void
terminal_screen_vte_eof (VteTerminal    *terminal,
                         TerminalScreen *screen)
{
  gtk_widget_destroy (GTK_WIDGET (screen));
}



static void
terminal_screen_vte_context_menu (TerminalWidget  *widget,
                                  GdkEvent        *event,
                                  TerminalScreen  *screen)
{
  g_signal_emit (G_OBJECT (screen), screen_signals[CONTEXT_MENU], 0, event);
}



static void
terminal_screen_vte_realize (VteTerminal    *terminal,
                             TerminalScreen *screen)
{
  vte_terminal_set_allow_bold (terminal, TRUE);
  terminal_screen_update_colors (TERMINAL_SCREEN (screen));
}



static void
terminal_screen_vte_map (VteTerminal    *terminal,
                         TerminalScreen *screen)
{
  terminal_screen_timer_background (TERMINAL_SCREEN (screen));
}



static void
terminal_screen_vte_selection_changed (VteTerminal    *terminal,
                                       TerminalScreen *screen)
{
  g_return_if_fail (VTE_IS_TERMINAL (terminal));
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  g_signal_emit (G_OBJECT (screen), screen_signals[SELECTION_CHANGED], 0);
}



static void
terminal_screen_vte_window_title_changed (VteTerminal    *terminal,
                                          TerminalScreen *screen)
{
  g_return_if_fail (VTE_IS_TERMINAL (terminal));
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  g_object_notify (G_OBJECT (screen), "title");
}



static gboolean
terminal_screen_timer_background (gpointer user_data)
{
  TerminalImageLoader *loader;
  TerminalBackground   background_mode;
  TerminalScreen      *screen = TERMINAL_SCREEN (user_data);
  GdkPixbuf           *image;
  gdouble              background_darkness;

  if (GTK_WIDGET_MAPPED (screen))
    {
      g_object_get (G_OBJECT (screen->preferences), "background-mode", &background_mode, NULL);

      if (background_mode == TERMINAL_BACKGROUND_SOLID)
        {
          vte_terminal_set_background_image (VTE_TERMINAL (screen->terminal), NULL);
          vte_terminal_set_background_saturation (VTE_TERMINAL (screen->terminal), 1.0);
          vte_terminal_set_background_transparent (VTE_TERMINAL (screen->terminal), FALSE);
        }
      else if (background_mode == TERMINAL_BACKGROUND_IMAGE)
        {
          vte_terminal_set_background_saturation (VTE_TERMINAL (screen->terminal), 1.0);
          vte_terminal_set_background_transparent (VTE_TERMINAL (screen->terminal), FALSE);

          loader = terminal_image_loader_get ();
          image = terminal_image_loader_load (loader,
                                              screen->terminal->allocation.width,
                                              screen->terminal->allocation.height);
          vte_terminal_set_background_image (VTE_TERMINAL (screen->terminal), image);
          if (GDK_IS_PIXBUF (image))
            g_object_unref (G_OBJECT (image));
          g_object_unref (G_OBJECT (loader));
        }
      else
        {
          g_object_get (G_OBJECT (screen->preferences), "background-darkness", &background_darkness, NULL);
          vte_terminal_set_background_image (VTE_TERMINAL (screen->terminal), NULL);
          vte_terminal_set_background_saturation (VTE_TERMINAL (screen->terminal), 1.0 - background_darkness);
          vte_terminal_set_background_transparent (VTE_TERMINAL (screen->terminal), TRUE);
        }
    }

  return FALSE;
}



static void
terminal_screen_timer_background_destroy (gpointer user_data)
{
  TERMINAL_SCREEN (user_data)->background_timer_id = 0;
}



/**
 * terminal_screen_new:
 *
 * Return value :
 **/
GtkWidget*
terminal_screen_new (void)
{
  return g_object_new (TERMINAL_TYPE_SCREEN, NULL);
}



/**
 * terminal_screen_launch_child:
 * @screen  : A #TerminalScreen.
 *
 * Starts the terminal child process.
 **/
void
terminal_screen_launch_child (TerminalScreen *screen)
{
  gboolean update;
  GError  *error = NULL;
  gchar   *command;
  gchar  **argv;
  gchar  **env;

  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

#ifdef DEBUG
  if (!GTK_WIDGET_REALIZED (screen))
    g_error ("Tried to launch command in a TerminalScreen that is not realized");
#endif

  if (!terminal_screen_get_child_command (screen, &command, &argv, &error))
    {
      g_error_free (error);
      return;
    }

  g_object_get (G_OBJECT (screen->preferences),
                "command-update-records", &update,
                NULL);

  env = terminal_screen_get_child_environment (screen);

  screen->pid = vte_terminal_fork_command (VTE_TERMINAL (screen->terminal),
                                           command, argv, env,
                                           screen->working_directory,
                                           update, update, update);

  g_strfreev (argv);
  g_strfreev (env);
  g_free (command);
}



/**
 * terminal_screen_set_custom_command:
 * @screen  : A #TerminalScreen.
 * @command :
 **/
void
terminal_screen_set_custom_command (TerminalScreen *screen,
                                    gchar         **command)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  if (G_UNLIKELY (screen->custom_command != NULL))
    g_strfreev (screen->custom_command);

  if (G_LIKELY (command != NULL && *command != NULL))
    screen->custom_command = g_strdupv (command);
  else
    screen->custom_command = NULL;
}



/**
 * terminal_screen_set_custom_title:
 * @screen  : A #TerminalScreen.
 * @title   : Title string.
 **/
void
terminal_screen_set_custom_title (TerminalScreen *screen,
                                  const gchar    *title)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  if (!exo_str_is_equal (screen->custom_title, title))
    {
      g_free (screen->custom_title);
      screen->custom_title = g_strdup (title != NULL ? title : "");
      g_object_notify (G_OBJECT (screen), "custom-title");
      g_object_notify (G_OBJECT (screen), "title");
    }
}



/**
 **/
void
terminal_screen_get_size (TerminalScreen *screen,
                          gint           *width_chars,
                          gint           *height_chars)
{
  *width_chars = VTE_TERMINAL (screen->terminal)->column_count;
  *height_chars = VTE_TERMINAL (screen->terminal)->row_count;
}



/**
 **/
void
terminal_screen_set_size (TerminalScreen *screen,
                          gint            width_chars,
                          gint            height_chars)
{
  vte_terminal_set_size (VTE_TERMINAL (screen->terminal), width_chars, height_chars);
}



/**
 * terminal_screen_force_resize_window:
 *
 * I don't like this way, but its required to work-around a Gtk+
 * bug (maybe also caused by a Vte bug, not sure).
 **/
void
terminal_screen_force_resize_window (TerminalScreen *screen,
                                     GtkWindow      *window,
                                     gint            force_columns,
                                     gint            force_rows)
{
  GtkRequisition terminal_requisition;
  GtkRequisition window_requisition;
  gint           width;
  gint           height;
  gint           columns;
  gint           rows;
  gint           xpad;
  gint           ypad;

  gtk_widget_set_size_request (screen->terminal, 2000, 2000);
  gtk_widget_size_request (GTK_WIDGET (window), &window_requisition);
  gtk_widget_size_request (screen->terminal, &terminal_requisition);

  width = window_requisition.width - terminal_requisition.width;
  height = window_requisition.height - terminal_requisition.height;

  if (force_columns < 0)
    columns = VTE_TERMINAL (screen->terminal)->column_count;
  else
    columns = force_columns;

  if (force_rows < 0)
    rows = VTE_TERMINAL (screen->terminal)->row_count;
  else
    rows = force_rows;

  vte_terminal_get_padding (VTE_TERMINAL (screen->terminal), &xpad, &ypad);

  width += xpad + VTE_TERMINAL (screen->terminal)->char_width * columns;
  height += ypad + VTE_TERMINAL (screen->terminal)->char_height * rows;

  if (GTK_WIDGET_MAPPED (window))
    gtk_window_resize (window, width, height);
  else
    gtk_window_set_default_size (window, width, height);

  gtk_widget_grab_focus (screen->terminal);
}



/**
 * terminal_screen_set_window_geometry_hints:
 *
 * I don't like this way, but its required to work-around a Gtk+
 * bug (maybe also caused by a Vte bug, not sure).
 **/
void
terminal_screen_set_window_geometry_hints (TerminalScreen *screen,
                                           GtkWindow      *window)
{
  GdkGeometry hints;
  gint        xpad;
  gint        ypad;

  vte_terminal_get_padding (VTE_TERMINAL (screen->terminal), &xpad, &ypad);

  hints.base_width = xpad;
  hints.base_height = ypad;
  hints.width_inc = VTE_TERMINAL (screen->terminal)->char_width;
  hints.height_inc = VTE_TERMINAL (screen->terminal)->char_height;
  hints.min_width = hints.base_width + hints.width_inc * 4;
  hints.min_height = hints.base_height + hints.height_inc * 2;

  gtk_window_set_geometry_hints (GTK_WINDOW (window),
                                 screen->terminal,
                                 &hints,
                                 GDK_HINT_RESIZE_INC
                                 | GDK_HINT_MIN_SIZE
                                 | GDK_HINT_BASE_SIZE);
}



/**
 * terminal_screen_get_title:
 * @screen      : A #TerminalScreen.
 *
 * Return value : The title to set for this terminal screen.
 *                The returned string should be freed when
 *                no longer needed.
 **/
gchar*
terminal_screen_get_title (TerminalScreen *screen)
{
  TerminalTitle mode;
  const gchar  *vte_title;
  gboolean      vte_workaround_title_bug;
  gchar        *window_title;
  gchar        *initial;
  gchar        *title;
  gchar        *tmp;

  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), NULL);

  if (G_UNLIKELY (*screen->custom_title != '\0'))
    return g_strdup (screen->custom_title);

  g_object_get (G_OBJECT (screen->preferences),
                "title-initial", &initial,
                "title-mode", &mode,
                "vte-workaround-title-bug", &vte_workaround_title_bug,
                NULL);

  vte_title = vte_terminal_get_window_title (VTE_TERMINAL (screen->terminal));
  window_title = (vte_title != NULL) ? g_strdup (vte_title) : NULL;

  /* work around VTE problem, see #10 for details */
  if (window_title != NULL && vte_workaround_title_bug)
    {
      /* remove initial title from the end of the dynamic title */
      tmp = g_strconcat (" - ", initial, NULL);
      while (g_str_has_suffix (window_title, tmp))
        window_title[strlen (window_title) - strlen (tmp)] = '\0';
      g_free (tmp);

      /* remove initial title from the end of the dynamic title */
      tmp = g_strconcat (initial, " - ", NULL);
      while (g_str_has_prefix (window_title, tmp))
        {
          memmove (window_title, window_title + strlen (tmp),
                   strlen (window_title) + 1 - strlen (tmp));
        }
      g_free (tmp);

#ifdef DEBUG
      g_print ("Terminal application specified title \"%s\", "
               "we've stripped that down to \"%s\"\n",
               vte_title, window_title);
#endif
    }

  switch (mode)
    {
    case TERMINAL_TITLE_REPLACE:
      if (window_title != NULL)
        title = g_strdup (window_title);
      else
        title = g_strdup (_("Untitled"));
      break;

    case TERMINAL_TITLE_PREPEND:
      if (window_title != NULL)
        title = g_strconcat (window_title, " - ", initial, NULL);
      else
        title = g_strdup (initial);
      break;

    case TERMINAL_TITLE_APPEND:
      if (window_title != NULL)
        title = g_strconcat (initial, " - ", window_title, NULL);
      else
        title = g_strdup (initial);
      break;

    case TERMINAL_TITLE_HIDE:
      title = g_strdup (initial);
      break;

    default:
      g_assert_not_reached ();
      title = NULL;
    }

  g_free (window_title);
  g_free (initial);

  return title;
}



/**
 * terminal_screen_get_working_directory:
 * @screen      : A #TerminalScreen.
 *
 * Determinies the working directory using various OS-specific mechanisms.
 *
 * Return value : The current working directory of @screen.
 **/
const gchar*
terminal_screen_get_working_directory (TerminalScreen *screen)
{
  gchar  buffer[4096 + 1];
  gchar *file;
  gchar *cwd;
  gint   length;

  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), NULL);

  if (screen->pid >= 0)
    {
      file = g_strdup_printf ("/proc/%d/cwd", screen->pid);
      length = readlink (file, buffer, sizeof (buffer));

      if (length > 0 && *buffer == '/')
        {
          buffer[length] = '\0';
          g_free (screen->working_directory);
          screen->working_directory = g_strdup (buffer);
        }
      else if (length == 0)
        {
          cwd = g_get_current_dir ();
          if (G_LIKELY (cwd != NULL))
            {
              if (chdir (file) == 0)
                {
                  g_free (screen->working_directory);
                  screen->working_directory = g_get_current_dir ();
                  chdir (cwd);
                }

              g_free (cwd);
            }
        }

      g_free (file);
    }

  return screen->working_directory;
}



/**
 * terminal_screen_set_working_directory:
 * @screen    : A #TerminalScreen.
 * @directory :
 **/
void
terminal_screen_set_working_directory (TerminalScreen *screen,
                                       const gchar    *directory)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));
  g_return_if_fail (directory != NULL);

  g_free (screen->working_directory);
  screen->working_directory = g_strdup (directory);
}



/**
 * terminal_screen_has_selection:
 * @screen      : A #TerminalScreen.
 *
 * Checks if the terminal currently contains selected text. Note that this is different from
 * determining if the terminal is the owner of any GtkClipboard items.
 *
 * Return value : %TRUE if part of the text in the terminal is selected.
 **/
gboolean
terminal_screen_has_selection (TerminalScreen *screen)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), FALSE);
  return vte_terminal_get_has_selection (VTE_TERMINAL (screen->terminal));
}



/**
 * terminal_screen_copy_clipboard:
 * @screen  : A #TerminalScreen.
 *
 * Places the selected text in the terminal in the #GDK_SELECTIN_CLIPBOARD selection.
 **/
void
terminal_screen_copy_clipboard (TerminalScreen *screen)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));
  vte_terminal_copy_clipboard (VTE_TERMINAL (screen->terminal));
}



/**
 * terminal_screen_paste_clipboard:
 * @screen  : A #TerminalScreen.
 *
 * Sends the contents of the #GDK_SELECTION_CLIPBOARD selection to the terminal's
 * child. If neccessary, the data is converted from UTF-8 to the terminal's current
 * encoding.
 **/
void
terminal_screen_paste_clipboard (TerminalScreen *screen)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));
  vte_terminal_paste_clipboard (VTE_TERMINAL (screen->terminal));
}



/**
 * terminal_screen_reset:
 * @screen  : A #TerminalScreen.
 * @clear   : %TRUE to also clear the terminal screen.
 *
 * Resets the terminal.
 **/
void
terminal_screen_reset (TerminalScreen *screen,
                       gboolean        clear)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));
  vte_terminal_reset (VTE_TERMINAL (screen->terminal), TRUE, clear);
}



/**
 * terminal_screen_im_append_menuitems:
 * @screen    : A #TerminalScreen.
 * @menushell : A #GtkMenuShell.
 *
 * Appends menu items for various input methods to the given @menushell.
 * The user can select one of these items to modify the input method
 * used by the terminal.
 **/
void
terminal_screen_im_append_menuitems (TerminalScreen *screen,
                                     GtkMenuShell   *menushell)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));
  g_return_if_fail (GTK_IS_MENU_SHELL (menushell));

  vte_terminal_im_append_menuitems (VTE_TERMINAL (screen->terminal), menushell);
}



/**
 * terminal_screen_get_restart_command:
 * @screen  : A #TerminalScreen.
 *
 * Return value:
 **/
GList*
terminal_screen_get_restart_command (TerminalScreen *screen)
{
  const gchar *directory;
  GList       *result = NULL;

  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), NULL);

  if (screen->custom_command != NULL)
    {
      result = g_list_append (result, g_strdup ("-e"));
      result = g_list_append (result, g_strjoinv (" ", screen->custom_command));
    }

  if (screen->custom_title != NULL)
    {
      result = g_list_append (result, g_strdup ("-t"));
      result = g_list_append (result, g_strdup (screen->custom_title));
    }

  directory = terminal_screen_get_working_directory (screen);
  if (G_LIKELY (directory != NULL))
    {
      result = g_list_append (result, g_strdup ("--working-directory"));
      result = g_list_append (result, g_strdup (directory));
    }

  return result;
}


