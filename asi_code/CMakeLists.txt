add_library(RadioEx SHARED 
    RadioEx.cpp 
    RadioEx.rc
	gta.lds
)
set_source_files_properties(gta.lds PROPERTIES EXTERNAL_OBJECT TRUE)

target_link_libraries(RadioEx PRIVATE wininet)
target_link_options(RadioEx PRIVATE "-Wl,--enable-stdcall-fixup")
target_link_libraries(RadioEx PRIVATE "-Wl,-Bstatic" minhook "-Wl,-Bdynamic")

set_target_properties(RadioEx PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "RadioEx"
)

# installs in CMAKE_INSTALL_PREFIX, assuming it points to the game dir (passed at configure time)
install(TARGETS RadioEx DESTINATION ".")
