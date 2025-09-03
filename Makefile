.PHONY: all
all: test_server

FORMATTABLE_C_FILES = \
	test_server.c

CC := gcc

C_FLAGS := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wredundant-decls

.PHONY: format
format: format.sh Makefile $(FORMATTABLE_C_FILES)
	@printf "Formatting      %s\n" "$(FORMATTABLE_C_FILES)"
	@./format.sh clang-format -i $(FORMATTABLE_C_FILES)

TEST_SERVER_C_FILES = test_server.c
test_server: Makefile $(TEST_SERVER_C_FILES)
	@printf "Compiling       %s\n" "$@"
	$(CC) -o $@ $(C_FLAGS) $(TEST_SERVER_C_FILES) -lzstd -lz
