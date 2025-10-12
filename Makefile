PWD := $(shell pwd)

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

SRC_DIR := $(PWD)
BUILD_DIR := $(PWD)/build
BUILD_FILES := $(SRC_DIR)/*.ko

$(shell mkdir -p $(BUILD_DIR))

# tab must be used as first charater before a command, can change to another prefix
.RECIPEPREFIX = >

kbuild:
>   $(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules
>   mv $(BUILD_FILES) $(BUILD_DIR)/.

clean:
>   $(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

check:
>   sudo ./check.sh

reinsmod:
>   sudo ./reinsmod.sh

# https://clang.llvm.org/docs/ClangFormat.html
# https://clang.llvm.org/docs/ClangFormatStyleOptions.html
format:
>   clang-format --style="{BasedOnStyle: google, IndentWidth: 4, ReflowComments: true}" -i -- **.c

app_us:
>   gcc us_oscillator.c -o ./build/us_oscillator

# do not associate targets with files
.PHONY: kbuild clean check format
