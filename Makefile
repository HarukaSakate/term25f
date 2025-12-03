TARGET = congestion_control

BPF_OBJ = ${TARGET}.bpf.o
USER_C = ${TARGET}.c
USER_SKEL = ${TARGET}.skel.h

LIBBPF_SRC = /home/l0gic/libbpf/src
LIBBPF_OBJ = $(LIBBPF_SRC)/libbpf.a

BPFTOOL = /usr/lib/linux-tools/6.8.0-88-generic/bpftool

CFLAGS = -g -O2 -Wall -I$(LIBBPF_SRC) -I$(LIBBPF_SRC)/.. -I.
LDFLAGS = -lelf -lz

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(USER_C) $(USER_SKEL)
	$(CC) $(CFLAGS) -o $@ $< $(LIBBPF_OBJ) $(LDFLAGS)

$(USER_SKEL): $(BPF_OBJ)
	$(BPFTOOL) gen skeleton $< > $@

$(BPF_OBJ): ${TARGET}.bpf.c
	clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -I$(LIBBPF_SRC) -c $< -o $@

clean:
	rm -f $(TARGET) $(BPF_OBJ) $(USER_SKEL)