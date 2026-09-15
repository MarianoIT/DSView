# This build enables exactly demo and dreamsourcelab-dslogic. Use bounded arrays
# instead of iterating across separately allocated linker-section globals;
# AddressSanitizer puts redzones between those globals on Mach-O.
file(READ "${SIGROK_SOURCE}/src/libsigrok-internal.h" header)
set(old "static const struct sr_dev_driver *name[] \\\n\t\t__attribute__((section (SR_DRIVER_LIST_SECTION), used, \\\n\t\t\taligned(sizeof(struct sr_dev_driver *))))")
string(FIND "${header}" "${old}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "Unexpected libsigrok driver registration macro")
endif()
string(REPLACE "${old}" "const struct sr_dev_driver *name[]" header "${header}")
file(WRITE "${SIGROK_SOURCE}/src/libsigrok-internal.h" "${header}")
file(READ "${SIGROK_SOURCE}/src/drivers.c" source)
string(REPLACE "SR_PRIV extern const struct sr_dev_driver *sr_driver_list__start[];\nSR_PRIV extern const struct sr_dev_driver *sr_driver_list__stop[];"
    "extern const struct sr_dev_driver *demo_driver_info_list[];\nextern const struct sr_dev_driver *dreamsourcelab_dslogic_driver_info_list[];" source "${source}")
set(old "for (const struct sr_dev_driver **drivers = sr_driver_list__start + 1;\n\t     drivers < sr_driver_list__stop; drivers++)\n\t\tg_array_append_val(array, *drivers);")
string(FIND "${source}" "${old}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "Unexpected libsigrok driver list implementation")
endif()
string(REPLACE "${old}" "g_array_append_vals(array, demo_driver_info_list, 1);\n\tg_array_append_vals(array, dreamsourcelab_dslogic_driver_info_list, 1);" source "${source}")
file(WRITE "${SIGROK_SOURCE}/src/drivers.c" "${source}")
