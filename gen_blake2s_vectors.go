//go:build ignore
// +build ignore

package main

import (
	"encoding/hex"
	"fmt"

	"golang.org/x/crypto/blake2s"
)

func main() {
	// Test 1: blake2s_256_empty
	h, _ := blake2s.New256(nil)
	sum := h.Sum(nil)
	fmt.Printf("blake2s_256_empty: %s\n", hex.EncodeToString(sum))

	// Test 2: blake2s_256_abc
	h, _ = blake2s.New256(nil)
	h.Write([]byte("abc"))
	sum = h.Sum(nil)
	fmt.Printf("blake2s_256_abc: %s\n", hex.EncodeToString(sum))

	// Test 3: blake2s_256_long — 200 bytes [0..199]
	data := make([]byte, 200)
	for i := range data {
		data[i] = byte(i)
	}
	h, _ = blake2s.New256(nil)
	h.Write(data)
	sum = h.Sum(nil)
	fmt.Printf("blake2s_256_long: %s\n", hex.EncodeToString(sum))

	// Test 4: blake2s_256_exact_block — 64 bytes [0..63]
	data = make([]byte, 64)
	for i := range data {
		data[i] = byte(i)
	}
	h, _ = blake2s.New256(nil)
	h.Write(data)
	sum = h.Sum(nil)
	fmt.Printf("blake2s_256_exact_block: %s\n", hex.EncodeToString(sum))

	// Test 5: blake2s_256_two_blocks — 128 bytes [0..127]
	data = make([]byte, 128)
	for i := range data {
		data[i] = byte(i)
	}
	h, _ = blake2s.New256(nil)
	h.Write(data)
	sum = h.Sum(nil)
	fmt.Printf("blake2s_256_two_blocks: %s\n", hex.EncodeToString(sum))

	// Test 6: blake2s_128_keyed — key=[0..31], data="test data for keyed blake2s"
	key := make([]byte, 32)
	for i := range key {
		key[i] = byte(i)
	}
	h128, _ := blake2s.New128(key)
	h128.Write([]byte("test data for keyed blake2s"))
	sum = h128.Sum(nil)
	fmt.Printf("blake2s_128_keyed: %s\n", hex.EncodeToString(sum))

	// Test 7: blake2s_128_keyed_empty — key=[0x80..0x9f], data=""
	for i := range key {
		key[i] = byte(i + 0x80)
	}
	h128, _ = blake2s.New128(key)
	sum = h128.Sum(nil)
	fmt.Printf("blake2s_128_keyed_empty: %s\n", hex.EncodeToString(sum))

	// Test 8: blake2s_128_keyed_long — key=[0,3,6..93], data=300 bytes [0..255,0..43]
	for i := range key {
		key[i] = byte(i * 3)
	}
	data = make([]byte, 300)
	for i := range data {
		data[i] = byte(i)
	}
	h128, _ = blake2s.New128(key)
	h128.Write(data)
	sum = h128.Sum(nil)
	fmt.Printf("blake2s_128_keyed_long: %s\n", hex.EncodeToString(sum))

	// Test 9: compute_mac1_key — pubkey=[1..32]
	pubkey := make([]byte, 32)
	for i := range pubkey {
		pubkey[i] = byte(i + 1)
	}
	label := append([]byte("mac1----"), pubkey...)
	h, _ = blake2s.New256(nil)
	h.Write(label)
	sum = h.Sum(nil)
	fmt.Printf("compute_mac1_key: %s\n", hex.EncodeToString(sum))

	// Test 10: recompute_mac1 — fake handshake init
	// serverPub[i] = i + 0x10, compute mac1key, then mac1 = blake2s-128(mac1key, buf[0:116])
	serverPub := make([]byte, 32)
	for i := range serverPub {
		serverPub[i] = byte(i + 0x10)
	}
	label2 := append([]byte("mac1----"), serverPub...)
	h, _ = blake2s.New256(nil)
	h.Write(label2)
	mac1key := h.Sum(nil)
	fmt.Printf("recompute_mac1_key: %s\n", hex.EncodeToString(mac1key))

	// Build a fake 148-byte handshake init packet
	buf := make([]byte, 148)
	// type = 1234567890 LE
	buf[0] = byte(1234567890 & 0xFF)
	buf[1] = byte((1234567890 >> 8) & 0xFF)
	buf[2] = byte((1234567890 >> 16) & 0xFF)
	buf[3] = byte((1234567890 >> 24) & 0xFF)
	for i := 4; i < 116; i++ {
		buf[i] = byte(i)
	}
	// buf[116:132] is MAC1 field, buf[132:148] is MAC2 field (zeros)
	// Compute MAC1 = BLAKE2s-128(mac1key, buf[0:116])
	h128, _ = blake2s.New128(mac1key)
	h128.Write(buf[0:116])
	mac1 := h128.Sum(nil)
	fmt.Printf("recompute_mac1: %s\n", hex.EncodeToString(mac1))

	// Test 11: incremental — same as test 3 (200 bytes)
	// Already computed above as blake2s_256_long
}
