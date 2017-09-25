/* plugins.c
 * plugin routines
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#ifdef HAVE_PLUGINS

#include <time.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <gmodule.h>

#include <wsutil/filesystem.h>
#include <wsutil/privileges.h>
#include <wsutil/file_util.h>
#include <wsutil/report_message.h>

#include <wsutil/plugins.h>
#include <wsutil/ws_printf.h> /* ws_debug_printf */

/* linked list of all plugins */
typedef struct _plugin {
    GModule        *handle;       /* handle returned by g_module_open */
    gchar          *name;         /* plugin name */
    gchar          *version;      /* plugin version */
    guint32         types;        /* bitmask of plugin types this plugin supports */
    struct _plugin *next;         /* forward link */
} plugin;

static plugin *plugin_list = NULL;

/*
 * Add a new plugin type.
 * Takes a callback routine as an argument; it is called for each plugin
 * we find, and handed a handle for the plugin, the name of the plugin,
 * and the version string for the plugin.  The plugin returns TRUE if
 * it's a plugin for that type and FALSE if not.
 */
typedef struct {
    const char *type;
    plugin_check_type_callback callback;
    guint type_val;
} plugin_type;

static GSList *plugin_types = NULL;

void
add_plugin_type(const char *type, plugin_check_type_callback callback)
{
    plugin_type *new_type;
    static guint type_val;

    if (type_val >= 32) {
        /*
         * There's a bitmask of types that a plugin provides, and it's
         * 32 bits, so we don't support types > 31.
         */
        report_failure("At most 32 plugin types can be supported, so the plugin type '%s' won't be supported.",
                       type);
        return;
    }
    new_type = (plugin_type *)g_malloc(sizeof (plugin_type));
    new_type->type = type;
    new_type->callback = callback;
    new_type->type_val = type_val;
    plugin_types = g_slist_append(plugin_types, new_type);
    type_val++;
}

/*
 * add a new plugin to the list
 */
static void
add_plugin(plugin *new_plug)
{
    plugin *pt_plug;

    pt_plug = plugin_list;
    if (!pt_plug) /* the list is empty */
    {
        plugin_list = new_plug;
    }
    else
    {
        while (1)
        {
            /* we found the last plugin in the list */
            if (pt_plug->next == NULL)
                break;

            pt_plug = pt_plug->next;
        }
        pt_plug->next = new_plug;
    }
}

static gboolean
check_if_plugin_exists(const char *name)
{
    for (plugin *p = plugin_list; p != NULL; p = p->next) {
        if (strcmp(p->name, name) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
call_plugin_callback(gpointer data, gpointer user_data)
{
    plugin_type *type = (plugin_type *)data;
    plugin *new_plug = (plugin *)user_data;

    if ((*type->callback)(new_plug->handle)) {
        /* The plugin supports this type */
        new_plug->types |= 1 << type->type_val;
    }
}

static void
plugins_scan_dir(const char *dirname, plugin_load_failure_mode mode)
{
#define FILENAME_LEN        1024
    WS_DIR        *dir;             /* scanned directory */
    WS_DIRENT     *file;            /* current file */
    const char    *name;
    gchar          filename[FILENAME_LEN];   /* current file name */
    GModule       *handle;          /* handle returned by g_module_open */
    gpointer       gp;
    plugin        *new_plug;
    gchar         *dot;

    if (!g_file_test(dirname, G_FILE_TEST_EXISTS) || !g_file_test(dirname, G_FILE_TEST_IS_DIR)) {
        return;
    }

    if ((dir = ws_dir_open(dirname, 0, NULL)) != NULL)
    {
        while ((file = ws_dir_read_name(dir)) != NULL)
        {
            name = ws_dir_get_name(file);

            /*
             * GLib 2.x defines G_MODULE_SUFFIX as the extension used on
             * this platform for loadable modules.
             */
            /* skip anything but files with G_MODULE_SUFFIX */
            dot = strrchr(name, '.');
            if (dot == NULL || strcmp(dot+1, G_MODULE_SUFFIX) != 0)
                continue;
#if WIN32
            if (strncmp(name, "nordic_ble.dll", 14) == 0)
                /*
                 * Skip the Nordic BLE Sniffer dll on WIN32 because
                 * the dissector has been added as internal.
                 */
                continue;
#endif
            g_snprintf(filename, FILENAME_LEN, "%s" G_DIR_SEPARATOR_S "%s",
                       dirname, name);

            /*
             * Check if the same name is already registered.
             */
            if (check_if_plugin_exists(name)) {
                /* Yes, it is. */
                if (mode == REPORT_LOAD_FAILURE) {
                    report_warning("The plugin '%s' was found "
                            "in multiple directories.\n", name);
                }
                continue;
            }

            if ((handle = g_module_open(filename, G_MODULE_BIND_LOCAL)) == NULL)
            {
                /*
                 * Only report load failures if we were asked to.
                 *
                 * XXX - we really should put different types of plugins
                 * (libwiretap, libwireshark) in different subdirectories,
                 * give libwiretap and libwireshark init routines that
                 * load the plugins, and have them scan the appropriate
                 * subdirectories so tha we don't even *try* to, for
                 * example, load libwireshark plugins in programs that
                 * only use libwiretap.
                 */
                if (mode == REPORT_LOAD_FAILURE) {
                    report_failure("Couldn't load module %s: %s", filename,
                                   g_module_error());
                }
                continue;
            }

            if (!g_module_symbol(handle, "version", &gp))
            {
                report_failure("The plugin %s has no version symbol", name);
                g_module_close(handle);
                continue;
            }

            new_plug = (plugin *)g_malloc(sizeof(plugin));
            new_plug->handle = handle;
            new_plug->name = g_strdup(name);
            new_plug->version = (char *)gp;
            new_plug->types = 0;
            new_plug->next = NULL;

            /*
             * Hand the plugin to each of the plugin type callbacks.
             */
            g_slist_foreach(plugin_types, call_plugin_callback, new_plug);

            /*
             * Does this dissector do anything useful?
             */
            if (new_plug->types == 0)
            {
                /*
                 * No.
                 *
                 * Only report this failure if we were asked to; it might
                 * just mean that it's a plugin type that this program
                 * doesn't support, such as a libwireshark plugin in
                 * a program that doesn't use libwireshark.
                 *
                 * XXX - we really should put different types of plugins
                 * (libwiretap, libwireshark) in different subdirectories,
                 * give libwiretap and libwireshark init routines that
                 * load the plugins, and have them scan the appropriate
                 * subdirectories so tha we don't even *try* to, for
                 * example, load libwireshark plugins in programs that
                 * only use libwiretap.
                 */
                if (mode == REPORT_LOAD_FAILURE) {
                    report_failure("The plugin '%s' has no registration routines",
                                   name);
                }
                g_module_close(handle);
                g_free(new_plug->name);
                g_free(new_plug);
                continue;
            }

            /*
             * OK, add it to the list of plugins.
             */
            add_plugin(new_plug);
        }
        ws_dir_close(dir);
    }
}


/*
 * Scan for plugins.
 */
void
scan_plugins(plugin_load_failure_mode mode)
{
    const char *plugin_dir;
    const char *name;
    char *plugin_dir_path;
    WS_DIR *dir;                /* scanned directory */
    WS_DIRENT *file;            /* current file */

    if (plugin_list == NULL)    /* only scan for plugins once */
    {
        /*
         * Scan the global plugin directory.
         * If we're running from a build directory, scan the "plugins"
         * subdirectory, as that's where plugins are located in an
         * out-of-tree build. If we find subdirectories scan those since
         * they will contain plugins in the case of an in-tree build.
         */
        plugin_dir = get_plugins_dir();
        if (plugin_dir == NULL)
        {
            /* We couldn't find the plugin directory. */
            return;
        }
        if (running_in_build_directory())
        {
            if ((dir = ws_dir_open(plugin_dir, 0, NULL)) != NULL)
            {
                plugins_scan_dir(plugin_dir, mode);
                while ((file = ws_dir_read_name(dir)) != NULL)
                {
                    name = ws_dir_get_name(file);
                    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                        continue;        /* skip "." and ".." */
                    /*
                     * Get the full path of a ".libs" subdirectory of that
                     * directory.
                     */
                    plugin_dir_path = g_strdup_printf(
                        "%s" G_DIR_SEPARATOR_S "%s" G_DIR_SEPARATOR_S ".libs",
                        plugin_dir, name);
                    if (test_for_directory(plugin_dir_path) != EISDIR) {
                        /*
                         * Either it doesn't refer to a directory or it
                         * refers to something that doesn't exist.
                         *
                         * Assume that means that the plugins are in
                         * the subdirectory of the plugin directory, not
                         * a ".libs" subdirectory of that subdirectory.
                         */
                        g_free(plugin_dir_path);
                        plugin_dir_path = g_strdup_printf("%s" G_DIR_SEPARATOR_S "%s",
                            plugin_dir, name);
                    }
                    plugins_scan_dir(plugin_dir_path, mode);
                    g_free(plugin_dir_path);
                }
                ws_dir_close(dir);
            }
        }
        else
        {
            plugins_scan_dir(get_plugins_dir_with_version(), mode);
        }

        /*
         * If the program wasn't started with special privileges,
         * scan the users plugin directory.  (Even if we relinquish
         * them, plugins aren't safe unless we've *permanently*
         * relinquished them, and we can't do that in Wireshark as,
         * if we need privileges to start capturing, we'd need to
         * reclaim them before each time we start capturing.)
         */
        if (!started_with_special_privs())
        {
            plugins_scan_dir(get_plugins_pers_dir_with_version(), mode);
        }
    }
}

/*
 * Iterate over all plugins, calling a callback with information about
 * the plugin.
 */
typedef struct {
    plugin  *pt_plug;
    GString *types;
    const char *sep;
} type_callback_info;

static void
add_plugin_type_description(gpointer data, gpointer user_data)
{
    plugin_type *type = (plugin_type *)data;
    type_callback_info *info = (type_callback_info *)user_data;

    /*
     * If the plugin handles this type, add the type to the list of types.
     */
    if (info->pt_plug->types & (1 << type->type_val)) {
        g_string_append_printf(info->types, "%s%s", info->sep, type->type);
        info->sep = ", ";
    }
}

WS_DLL_PUBLIC void
plugins_get_descriptions(plugin_description_callback callback, void *user_data)
{
    type_callback_info info;

    info.types = NULL; /* Certain compiler suites need a init state for this variable */
    for (info.pt_plug = plugin_list; info.pt_plug != NULL;
         info.pt_plug = info.pt_plug->next)
    {
        info.sep = "";
        info.types = g_string_new("");

        /*
         * Build a list of all the plugin types.
         */
        g_slist_foreach(plugin_types, add_plugin_type_description, &info);

        /*
         * And hand the information to the callback.
         */
        callback(info.pt_plug->name, info.pt_plug->version, info.types->str,
                 g_module_name(info.pt_plug->handle), user_data);

        g_string_free(info.types, TRUE);
    }
}

static void
print_plugin_description(const char *name, const char *version,
                         const char *description, const char *filename,
                         void *user_data _U_)
{
    ws_debug_printf("%s\t%s\t%s\t%s\n", name, version, description, filename);
}

void
plugins_dump_all(void)
{
    plugins_get_descriptions(print_plugin_description, NULL);
}

static void
free_plugin_type(gpointer p, gpointer user_data _U_)
{
    g_free(p);
}

void
plugins_cleanup(void)
{
    plugin* cur, *next;

    for (cur = plugin_list; cur != NULL; cur = next) {
        next = cur->next;
        g_free(cur->name);
        g_free(cur);
    }

    g_slist_foreach(plugin_types, free_plugin_type, NULL);
    g_slist_free(plugin_types);
}

#endif /* HAVE_PLUGINS */

/*
 * Editor modelines
 *
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
