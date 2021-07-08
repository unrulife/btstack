#!/usr/bin/env python3
#
# Create project files for all BTstack embedded examples in local port/esp32 folder

import os
import shutil
import sys
import time
import subprocess

mk_template = '''#
# BTstack example 'EXAMPLE' for ESP32 port
#
# Generated by TOOL
# On DATE

PROJECT_NAME := EXAMPLE

include $(IDF_PATH)/make/project.mk
'''

component_mk_gatt_add_on = '''
# app depends on compiled gatt db
EXAMPLE.o: EXAMPLE.h

# rule to compile gatt db
EXAMPLE.h: $(COMPONENT_PATH)/EXAMPLE.gatt
\t$(IDF_PATH)/components/btstack/tool/compile_gatt.py $^ $@

# remove compiled gatt db on clean
COMPONENT_EXTRA_CLEAN = EXAMPLE.h
'''

example_cmake_template = '''
# BTstack example 'EXAMPLE' for ESP32 port
#
# Generated by TOOL
# On DATE

# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(EXAMPLE)
'''

main_cmake_template = '''
idf_component_register(
        SRCS MAIN_FILES
        INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}")
'''

main_cmake_gatt_add_on = '''
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/EXAMPLE.h
            COMMAND ${CMAKE_SOURCE_DIR}/../../../../tool/compile_gatt.py ${COMPONENT_DIR}/EXAMPLE.gatt ${CMAKE_CURRENT_BINARY_DIR}/EXAMPLE.h
            DEPENDS EXAMPLE.gatt
            VERBATIM
    )
    add_custom_target(GATT_DB DEPENDS EXAMPLE.h)
    add_dependencies(${COMPONENT_LIB} GATT_DB)
endif()
'''

def create_examples(script_path, suffix):
    # path to examples
    examples_embedded = script_path + "/../../example/"

    # path to samples
    example_folder = script_path + "/example" + suffix + "/"

    print("Creating examples folder")
    if not os.path.exists(example_folder):
        os.makedirs(example_folder)

    print("Creating examples in examples folder")

    # iterate over btstack examples
    for file in os.listdir(examples_embedded):
        if not file.endswith(".c"):
            continue
        if file in ['panu_demo.c', 'sco_demo_util.c', 'ant_test.c']:
            continue

        example = file[:-2]
        gatt_path = examples_embedded + example + ".gatt"

        # create folder
        apps_folder = example_folder + example + "/"
        if os.path.exists(apps_folder):
            shutil.rmtree(apps_folder)
        os.makedirs(apps_folder)

        # copy files
        for item in ['sdkconfig', 'set_port.sh']:
            src = script_path + '/template/' + item
            if item == 'sdkconfig':
                src = src + suffix
            dst = apps_folder + '/' + item
            shutil.copyfile(src, dst)

        # mark set_port.sh as executable
        os.chmod(apps_folder + '/set_port.sh', 0o755)

        # create Makefile file
        with open(apps_folder + "Makefile", "wt") as fout:
            fout.write(mk_template.replace("EXAMPLE", example).replace("TOOL", script_path).replace("DATE",time.strftime("%c")))

        # create CMakeLists.txt file
        with open(apps_folder + "CMakeLists.txt", "wt") as fout:
            fout.write(example_cmake_template.replace("EXAMPLE", example).replace("TOOL", script_path).replace("DATE",time.strftime("%c")))

        # create main folder
        main_folder = apps_folder + "main/"
        if not os.path.exists(main_folder):
            os.makedirs(main_folder)

        # copy main file
        shutil.copyfile(script_path + '/template/main/main.c', apps_folder + "/main/main.c")

        # copy example file
        main_files = '"mainc." "' + example + '.c"'
        shutil.copyfile(examples_embedded + file, apps_folder + "/main/" + example + ".c")

        # add sco_demo_util.c for audio examples
        if example in ['hfp_ag_demo','hfp_hf_demo', 'hsp_ag_demo', 'hsp_hs_demo']:
            shutil.copy(examples_embedded + 'sco_demo_util.c', apps_folder + '/main/')
            shutil.copy(examples_embedded + 'sco_demo_util.h', apps_folder + '/main/')
            main_files += ' "sco_demo_util.c"'

        # add component.mk file to main folder
        main_component_mk = apps_folder + "/main/component.mk"
        shutil.copyfile(script_path + '/template/main/component.mk', main_component_mk)

        # create CMakeLists.txt file
        main_cmake_file = apps_folder + "/main/CMakeLists.txt"
        with open(main_cmake_file, "wt") as fout:
            fout.write(main_cmake_template.replace("EXAMPLE_FILES", main_files))

        # add rules to compile gatt db if .gatt file is present
        gatt_path = examples_embedded + example + ".gatt"
        if os.path.exists(gatt_path):
            shutil.copy(gatt_path, apps_folder + "/main/" + example + ".gatt")
            with open(main_component_mk, "a") as fout:
                fout.write(component_mk_gatt_add_on.replace("EXAMPLE", example))
            with open(main_cmake_file, "a") as fout:
                fout.write(main_cmake_gatt_add_on.replace("EXAMPLE", example))
            print("- %s including GATT DB compilation rules" % example)
        else:
            print("- %s" % example)

if __name__ == '__main__':
    # get script path
    script_path = os.path.abspath(os.path.dirname(sys.argv[0]))
    suffix = ''
    if len(sys.argv) > 1:
        suffix = sys.argv[1]
    create_examples(script_path, suffix)


