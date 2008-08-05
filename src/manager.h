/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2008  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#define MANAGER_INTERFACE "org.bluez.Manager"

dbus_bool_t manager_init(DBusConnection *conn, const char *path);
void manager_cleanup(DBusConnection *conn, const char *path);

struct adapter *manager_find_adapter(const bdaddr_t *sba);
struct adapter *manager_find_adapter_by_path(const char *path);
struct adapter *manager_find_adapter_by_id(int id);
int manager_register_adapter(int id);
int manager_unregister_adapter(int id);
int manager_start_adapter(int id);
int manager_stop_adapter(int id);
int manager_get_default_adapter();
void manager_set_default_adapter(int id);
int manager_update_adapter(uint16_t id);
int manager_get_adapter_class(uint16_t dev_id, uint8_t *cls);
int manager_set_adapter_class(uint16_t dev_id, uint8_t *cls);