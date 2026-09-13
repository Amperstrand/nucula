idf_component_register(SRCS "rc522.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver)

set_source_files_properties("rc522.c" PROPERTIES COMPILE_OPTIONS "-O2")
