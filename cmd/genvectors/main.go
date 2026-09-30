// Command genvectors 生成协议测试向量（固定输入 → 确定性输出），
// 供 Go（crypto_test.go）与 C++（protocol_test.cpp）两端交叉验证。
//
// 用法：go run ./cmd/genvectors   → 输出 internal/protocol/testdata/vectors.json
package main

import (
	"crypto/aes"
	"crypto/cipher"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	"ratc2/internal/protocol"
)

// 固定输入（必须保持稳定，改动即破坏两端向量）
var (
	psk           = bytes01to(protocol.PSKLen)
	clientNonce   = bytes01to(protocol.ClientNonceLen)
	serverNonce   = bytes01to(protocol.ServerNonceLen)
	encKey        = bytes01to(protocol.SessionKeyLen)
	encNonce      = []byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC}
	plaintexts    = [][]byte{
		[]byte(""),
		[]byte("hello"),
		[]byte("the quick brown fox jumps over the lazy dog 0123456789"),
	}
)

func bytes01to(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(i + 1)
	}
	return b
}

// 用固定 nonce 加密（GCM 手动拼装），生成确定性密文
func sealFixed(key, nonce, plaintext []byte) ([]byte, error) {
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return nil, err
	}
	sealed := gcm.Seal(nil, nonce, plaintext, nil)
	out := make([]byte, 0, len(nonce)+len(sealed))
	out = append(out, nonce...)
	out = append(out, sealed...)
	return out, nil
}

type vector struct {
	SessionKey       string   `json:"session_key"`
	Sealed           []string `json:"sealed"`            // 每个明文对应的 nonce+ct+tag（hex）
	PlaintextHex     []string `json:"plaintext_hex"`     // 明文（hex，便于 C++ 引用）
	Psk              string   `json:"psk"`
	ClientNonce      string   `json:"client_nonce"`
	ServerNonce      string   `json:"server_nonce"`
	EncKey           string   `json:"enc_key"`
	EncNonce         string   `json:"enc_nonce"`
	HeaderExampleHex string   `json:"header_example_hex"` // 20B 头示例
}

func main() {
	sk := protocol.DeriveSessionKey(psk, clientNonce, serverNonce)
	v := vector{
		SessionKey:   hex.EncodeToString(sk),
		Psk:          hex.EncodeToString(psk),
		ClientNonce:  hex.EncodeToString(clientNonce),
		ServerNonce:  hex.EncodeToString(serverNonce),
		EncKey:       hex.EncodeToString(encKey),
		EncNonce:     hex.EncodeToString(encNonce),
		HeaderExampleHex: "5a5a01" + "01" + // magic(2) version(1) flags(1, encrypted)
			"01000000" + // seq=1（little-endian）
			"00000000" + // timestamp=0（固定便于比对）
			"0101" + // cmd=CMD_HELLO(0x0101)
			"00" + // plugin=0
			"00" + // status=0
			"00000000", // payload_len=0（示例头，不含 payload）
	}
	for i, pt := range plaintexts {
		sealed, err := sealFixed(encKey, encNonce, pt)
		if err != nil {
			fmt.Fprintln(os.Stderr, "seal error:", err)
			os.Exit(1)
		}
		v.Sealed = append(v.Sealed, hex.EncodeToString(sealed))
		v.PlaintextHex = append(v.PlaintextHex, hex.EncodeToString(pt))
		_ = i
	}

	// 写 testdata/vectors.json
	dir := filepath.Join("internal", "protocol", "testdata")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	data, _ := json.MarshalIndent(v, "", "  ")
	out := filepath.Join(dir, "vectors.json")
	if err := os.WriteFile(out, append(data, '\n'), 0o644); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println("wrote", out)
	fmt.Println("session_key:", v.SessionKey)
}
