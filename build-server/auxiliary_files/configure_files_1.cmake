# Defining variables
set(MUMBLE_SERVER_BINARY_NAME "mumble-server")
set(MUMBLE_BUILD_YEAR "2026")
set(MUMBLE_INSTALL_ABS_EXECUTABLEDIR "/usr/local/bin")
set(MUMBLE_INSTALL_ABS_SYSCONFDIR "/usr/local/etc/mumble")

# Configuring files
configure_file("/src/auxiliary_files/config_files/mumble-server.service.in" "/build/auxiliary_files/mumble-server.service" @ONLY)
configure_file("/src/auxiliary_files/config_files/mumble-server.tmpfiles.in" "/build/auxiliary_files/mumble-server.tmpfiles" @ONLY)
configure_file("/src/auxiliary_files/run_scripts/mumble-server-user-wrapper.in" "/build/auxiliary_files/mumble-server-user-wrapper" @ONLY)
