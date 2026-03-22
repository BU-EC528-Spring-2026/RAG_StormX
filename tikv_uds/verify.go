package main

import (
	"context"
	"encoding/binary"
	"fmt"

	"github.com/tikv/client-go/v2/rawkv"
)

func main() {
	cli, err := rawkv.NewClientWithOpts(context.Background(), []string{"127.0.0.1:2379"})
	if err != nil {
		panic(err)
	}
	defer cli.Close()

	// Check key 0, which KVTest.cpp writes in the first iteration
	keyBuf := make([]byte, 8)
	binary.LittleEndian.PutUint64(keyBuf, 0)

	fullKey := append([]byte("spann:"), keyBuf...)

	val, err := cli.Get(context.Background(), fullKey)
	if err != nil {
		fmt.Println("Error reading from TiKV:", err)
		return
	}

	if val == nil {
		fmt.Println("Key not found! The data was NOT successfully written to TiKV.")
	} else {
		fmt.Printf("Success! Found key 0. Value size: %d bytes\n", len(val))
	}

	// Check key 1023 (the last key)
	binary.LittleEndian.PutUint64(keyBuf, 1023)
	fullKey = append([]byte("spann:"), keyBuf...)
	val, err = cli.Get(context.Background(), fullKey)
	if val != nil {
		fmt.Printf("Success! Found key 1023. Value size: %d bytes\n", len(val))
	} else {
		fmt.Println("Key 1023 not found!")
	}
}
