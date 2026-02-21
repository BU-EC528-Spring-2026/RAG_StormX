package main
import (
	"bufio"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/pingcap/kvproto/pkg/kvrpcpb"
	"github.com/tikv/client-go/v2/rawkv"
)

const (
	magicReq uint32 = 0x53505447 // 'SPTG' in ASCII, used in the header
	magicRes uint32 = 0x52535054 // 'RSPT' in ASCII, used in the header

	//opcodes
	opGet      uint16 = 1
	opMultiGet uint16 = 2
	opPut      uint16 = 3
	opDelete   uint16 = 4
	opHealth   uint16 = 5

	//statuses
	statusOK       int32 = 0
	statusNotFound int32 = 1
	statusTimeout  int32 = 2
	statusError    int32 = 3
)

type server struct { 			// state for the sidecar
	cli         *rawkv.Client 	// Holds the active connection client to the TiKV cluster
	namespace   []byte 		// Stores a prefix added to all keys to prevent collisions in TiKV
	maxBatch    int 		// Stores the maximum number of keys to request in a single TiKV batch
	workerLimit chan struct{} 	// Buffered channel acting as a semaphore to limit concurrency issues 
}

func getenvDefault(key, def string) string {
	v := os.Getenv(key)
	if v == "" {
		return def
	}
	return v
}

func parsePDAddrs(s string) []string {
	parts := strings.Split(s, ",")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

func mustAtoi(s string) int {
	v, err := strconv.Atoi(s)
	if err != nil {
		panic(err)
	}
	return v
}

func (sv *server) keyWithNS(k8 []byte) []byte {
	if len(sv.namespace) == 0 {
		return k8
	}
	out := make([]byte, 0, len(sv.namespace)+len(k8))
	out = append(out, sv.namespace...)
	out = append(out, k8...)
	return out
}

func readFull(r *bufio.Reader, b []byte) error {
	_, err := io.ReadFull(r, b)
	return err
}

func writeFull(w *bufio.Writer, b []byte) error {
	_, err := w.Write(b)
	return err
}

func (sv *server) handleConn(c net.Conn) {
	defer c.Close()

	uc, ok := c.(*net.UnixConn)
	if ok {
		_ = uc.SetReadBuffer(4 << 20)
		_ = uc.SetWriteBuffer(4 << 20)
	}

	r := bufio.NewReaderSize(c, 4<<20)
	w := bufio.NewWriterSize(c, 4<<20)

	hdr := make([]byte, 24) // magic(4) op(2) flags(2) reqid(8) timeout_us(8)
	for {
		if err := readFull(r, hdr); err != nil {
			if errors.Is(err, io.EOF) {
				return
			}
			return
		}
		m := binary.LittleEndian.Uint32(hdr[0:4])
		if m != magicReq {
			return
		}
		op := binary.LittleEndian.Uint16(hdr[4:6])
		reqID := binary.LittleEndian.Uint64(hdr[8:16])
		timeoutUS := binary.LittleEndian.Uint64(hdr[16:24])
		fmt.Printf("Received reqID: %d, raw timeoutUS: %d\n", reqID, timeoutUS)


		const maxSafeUS = 9223372036854775 

		if timeoutUS >= maxSafeUS {
			_ = c.SetDeadline(time.Time{})
		} else {
			deadline := time.Now().Add(time.Duration(timeoutUS) * time.Microsecond)
			_ = c.SetDeadline(deadline)
		}

		switch op {
		case opHealth:
			sv.writeStatus(w, reqID, statusOK, nil)
			if err := w.Flush(); err != nil {
				return
			}
			continue
		case opGet:
			if err := sv.handleGet(r, w, reqID, timeoutUS); err != nil {
				return
			}
		case opMultiGet:
			if err := sv.handleMultiGet(r, w, reqID, timeoutUS); err != nil {
				return
			}
		case opPut:
			if err := sv.handlePut(r, w, reqID, timeoutUS); err != nil {
				return
			}
		case opDelete:
			if err := sv.handleDelete(r, w, reqID, timeoutUS); err != nil {
				return
			}
		default:
			_ = sv.writeStatus(w, reqID, statusError, []byte("unknown op"))
			_ = w.Flush()
			return
		}

		if err := w.Flush(); err != nil {
			return
		}
	}
}

func (sv *server) writeStatus(w *bufio.Writer, reqID uint64, st int32, payload []byte) error {
	resHdr := make([]byte, 16) // magic(4) reqid(8) status(4)
	binary.LittleEndian.PutUint32(resHdr[0:4], magicRes)
	binary.LittleEndian.PutUint64(resHdr[4:12], reqID)
	binary.LittleEndian.PutUint32(resHdr[12:16], uint32(st))
	if err := writeFull(w, resHdr); err != nil {
		return err
	}
	if payload == nil {
		payload = []byte{}
	}
	lenBuf := make([]byte, 4)
	binary.LittleEndian.PutUint32(lenBuf, uint32(len(payload)))
	if err := writeFull(w, lenBuf); err != nil {
		return err
	}
	if len(payload) > 0 {
		if err := writeFull(w, payload); err != nil {
			return err
		}
	}
	return nil
}

func (sv *server) handleGet(r *bufio.Reader, w *bufio.Writer, reqID uint64, timeoutUS uint64) error {
	keyBuf := make([]byte, 8)
	if err := readFull(r, keyBuf); err != nil {
		return err
	}
	fullKey := sv.keyWithNS(keyBuf)

	var ctx context.Context
	var cancel context.CancelFunc

	if timeoutUS >= 9223372036854775 {
		ctx, cancel = context.WithCancel(context.Background()) // No timeout
	} else {
		ctx, cancel = context.WithTimeout(context.Background(), time.Duration(timeoutUS)*time.Microsecond)
	}
	defer cancel()	

	defer cancel()

	val, err := sv.cli.Get(ctx, fullKey)
	if err != nil {
		if errors.Is(err, context.DeadlineExceeded) {
			return sv.writeStatus(w, reqID, statusTimeout, []byte("timeout"))
		}
		return sv.writeStatus(w, reqID, statusError, []byte(err.Error()))
	}
	if val == nil {
		return sv.writeStatus(w, reqID, statusNotFound, nil)
	}

	payload := make([]byte, 4+len(val))
	binary.LittleEndian.PutUint32(payload[0:4], uint32(len(val)))
	copy(payload[4:], val)
	return sv.writeStatus(w, reqID, statusOK, payload)
}

func (sv *server) handleMultiGet(r *bufio.Reader, w *bufio.Writer, reqID uint64, timeoutUS uint64) error {
	nBuf := make([]byte, 4)
	if err := readFull(r, nBuf); err != nil {
		return err
	}
	n := int(binary.LittleEndian.Uint32(nBuf))
	if n < 0 || n > 1_000_000 {
		return sv.writeStatus(w, reqID, statusError, []byte("invalid n"))
	}
	if n == 0 {
		payload := make([]byte, 4)
		binary.LittleEndian.PutUint32(payload[0:4], 0)
		return sv.writeStatus(w, reqID, statusOK, payload)
	}

	keysRaw := make([][]byte, n)
	for i := 0; i < n; i++ {
		k := make([]byte, 8)
		if err := readFull(r, k); err != nil {
			return err
		}
		keysRaw[i] = sv.keyWithNS(k)
	}

	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(timeoutUS)*time.Microsecond)
	defer cancel()

	results := make([][]byte, n)

	for start := 0; start < n; start += sv.maxBatch {
		end := start + sv.maxBatch
		if end > n {
			end = n
		}

		sv.workerLimit <- struct{}{}
		err := func() error {
			defer func() { <-sv.workerLimit }()
			vals, err := sv.cli.BatchGet(ctx, keysRaw[start:end])
			if err != nil {
				return err
			}
			for i := start; i < end; i++ {
				v := vals[i-start]
				if v != nil {
					results[i] = v
				}
			}
			return nil
		}()
		if err != nil {
			if errors.Is(err, context.DeadlineExceeded) {
				return sv.writeStatus(w, reqID, statusTimeout, []byte("timeout"))
			}
			return sv.writeStatus(w, reqID, statusError, []byte(err.Error()))
		}
	}

	size := 4
	for i := 0; i < n; i++ {
		size += 1
		if results[i] != nil {
			size += 4 + len(results[i])
		}
	}
	payload := make([]byte, size)
	binary.LittleEndian.PutUint32(payload[0:4], uint32(n))
	off := 4
	for i := 0; i < n; i++ {
		if results[i] != nil {
			payload[off] = 1
			off++
			binary.LittleEndian.PutUint32(payload[off:off+4], uint32(len(results[i])))
			off += 4
			copy(payload[off:off+len(results[i])], results[i])
			off += len(results[i])
		} else {
			payload[off] = 0
			off++
		}
	}
	return sv.writeStatus(w, reqID, statusOK, payload)
}

func (sv *server) handlePut(r *bufio.Reader, w *bufio.Writer, reqID uint64, timeoutUS uint64) error {
	keyBuf := make([]byte, 8)
	if err := readFull(r, keyBuf); err != nil {
		return err
	}
	vlenBuf := make([]byte, 4)
	if err := readFull(r, vlenBuf); err != nil {
		return err
	}
	vlen := int(binary.LittleEndian.Uint32(vlenBuf))
	if vlen < 0 || vlen > (512<<20) {
		return sv.writeStatus(w, reqID, statusError, []byte("invalid vlen"))
	}
	val := make([]byte, vlen)
	if vlen > 0 {
		if err := readFull(r, val); err != nil {
			return err
		}
	}

	fullKey := sv.keyWithNS(keyBuf)

	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(timeoutUS)*time.Microsecond)
	defer cancel()

	if err := sv.cli.Put(ctx, fullKey, val); err != nil {
		if errors.Is(err, context.DeadlineExceeded) {
			return sv.writeStatus(w, reqID, statusTimeout, []byte("timeout"))
		}
		return sv.writeStatus(w, reqID, statusError, []byte(err.Error()))
	}
	return sv.writeStatus(w, reqID, statusOK, nil)
}

func (sv *server) handleDelete(r *bufio.Reader, w *bufio.Writer, reqID uint64, timeoutUS uint64) error {
	keyBuf := make([]byte, 8)
	if err := readFull(r, keyBuf); err != nil {
		return err
	}
	fullKey := sv.keyWithNS(keyBuf)

	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(timeoutUS)*time.Microsecond)
	defer cancel()

	if err := sv.cli.Delete(ctx, fullKey); err != nil {
		if errors.Is(err, context.DeadlineExceeded) {
			return sv.writeStatus(w, reqID, statusTimeout, []byte("timeout"))
		}
		return sv.writeStatus(w, reqID, statusError, []byte(err.Error()))
	}
	return sv.writeStatus(w, reqID, statusOK, nil)
}

func main() {
	pd := getenvDefault("TIKV_PD_ADDRS", "127.0.0.1:2379")
	socketPath := getenvDefault("SPTAG_TIKV_UDS", "/tmp/sptag_tikv.sock")
	ns := getenvDefault("SPTAG_TIKV_NAMESPACE", "spann:")
	maxBatch := mustAtoi(getenvDefault("SPTAG_TIKV_MAXBATCH", "256"))
	workers := mustAtoi(getenvDefault("SPTAG_TIKV_WORKERS", "4"))
	gmp := mustAtoi(getenvDefault("GOMAXPROCS", "8"))

	runtime.GOMAXPROCS(gmp)

	_ = os.Remove(socketPath)
	addr, err := net.ResolveUnixAddr("unix", socketPath)
	if err != nil {
		panic(err)
	}
	ln, err := net.ListenUnix("unix", addr)
	if err != nil {
		panic(err)
	}
	if err := os.Chmod(socketPath, 0777); err != nil {
		panic(err)
	}

	cli, err := rawkv.NewClientWithOpts(
	    context.Background(), 
	    parsePDAddrs(pd), 
	    rawkv.WithAPIVersion(kvrpcpb.APIVersion_V1),
	)	

	if err != nil {
		panic(err)
	}

	sv := &server{
		cli:         cli,
		namespace:   []byte(ns),
		maxBatch:    maxBatch,
		workerLimit: make(chan struct{}, workers),
	}

	fmt.Printf("tikv-uds: pd=%s uds=%s ns=%s maxBatch=%d workers=%d GOMAXPROCS=%d\n",
		pd, socketPath, ns, maxBatch, workers, gmp)

	var wg sync.WaitGroup
	for {
		c, err := ln.Accept()
		if err != nil {
			break
		}
		wg.Add(1)
		go func() {
			defer wg.Done()
			sv.handleConn(c)
		}()
	}
	wg.Wait()
}
