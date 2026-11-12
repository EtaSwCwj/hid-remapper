# Workaround for peripherals (e.g. Redbean A13S) that omit characteristic
# declarations from Find Information responses, which makes gatt_dm fail
# with -ESRCH. Patches the nRF Connect SDK source at configure time.
set(_gatt_dm_src ${ZEPHYR_NRF_MODULE_DIR}/subsys/bluetooth/gatt_dm.c)
file(READ ${_gatt_dm_src} _gatt_dm_content)
set(_gatt_dm_old [==[
	cur_attr = attr_find_by_handle(dm, attr->handle);
	if (!cur_attr) {
		/* We should never be here is the server is working properly */
		discovery_complete_error(dm, -ESRCH);
		return BT_GATT_ITER_STOP;
	}
]==])
set(_gatt_dm_new [==[
	cur_attr = attr_find_by_handle(dm, attr->handle);
	if (!cur_attr) {
		/* Workaround: some peripherals omit characteristic declarations
		 * from Find Information responses. Add the declaration here and
		 * keep the attribute array sorted by handle.
		 */
		cur_attr = attr_store(dm, attr, sizeof(struct bt_gatt_chrc));
		if (!cur_attr) {
			discovery_complete_error(dm, -ENOMEM);
			return BT_GATT_ITER_STOP;
		}
		struct bt_gatt_dm_attr tmp_attr = *cur_attr;
		size_t ins = dm->cur_attr_id - 1;

		while (ins > 0 && dm->attrs[ins - 1].handle > tmp_attr.handle) {
			dm->attrs[ins] = dm->attrs[ins - 1];
			ins--;
		}
		dm->attrs[ins] = tmp_attr;
		cur_attr = &dm->attrs[ins];
	}
]==])
string(FIND "${_gatt_dm_content}" "Workaround: some peripherals omit" _gatt_dm_already)
if(_gatt_dm_already EQUAL -1)
    string(REPLACE "${_gatt_dm_old}" "${_gatt_dm_new}" _gatt_dm_patched "${_gatt_dm_content}")
    if("${_gatt_dm_patched}" STREQUAL "${_gatt_dm_content}")
        message(FATAL_ERROR "gatt_dm.c not patched")
    endif()
    file(WRITE ${_gatt_dm_src} "${_gatt_dm_patched}")
    message(STATUS "patched gatt_dm.c")
endif()
