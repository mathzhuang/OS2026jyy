NAME := $(shell basename $(PWD))
export MODULE := M6
CFLAGS += -lpthread
# gpt.c 使用了 expf/sqrtf/tanhf 等数学函数，链接时需要 libm
LDFLAGS += -lm

all: $(NAME)

include ../oslab.mk
include ../.shadow/oslab.mk
