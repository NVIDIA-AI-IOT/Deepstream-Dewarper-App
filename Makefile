################################################################################
# Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA Corporation and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA Corporation is strictly prohibited.
#
################################################################################

CC:= gcc
TARGET_NAME:= deepstream-dewarper-app

INSTALL_DIR:=/usr/bin

SRCS:= 	deepstream_dewarper_test.c

INC_PATHS:= DS_SRC_ROOT_DIR/include \
	    DS_SRC_ROOT_DIR/src/gst-utils/gstnvdshelper \

CFLAGS+= -g -Wall -Werror

LIBS+= -lm \
	-LDS_BUILD_ROOT_DIR/src/utils/nvdsmeta -lnvds_meta \
  -LDS_BUILD_ROOT_DIR/src/gst-utils/gst-nvdsmeta -lnvdsgst_meta \
	-LDS_BUILD_ROOT_DIR/src/gst-utils/gstnvdshelper -lnvdsgst_helper

RELEASE_SRC_DIR_NAME:=apps/sample_apps/deepstream-dewarper-test
RELEASE_SRC_FILES:= deepstream_dewarper_test.c=deepstream_dewarper_test.c \
  Makefile.public=Makefile \
  config_dewarper.txt=config_dewarper.txt \
  config_dewarper_perspective.txt=config_dewarper_perspective.txt \
  csv_files=csv_files \
  README=README

PACKAGE_BINARY_IN_DS:=1
IS_APP:=1
#APP_PKG_DIR:=sources/apps/sample_apps/deepstream-dewarper-test

NEEDS_GST:=1

include ../../../../Rules.mk
