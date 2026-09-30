package protocol

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// 测试向量来自 testdata/vectors.json（cmd/genvectors 生成，固定输入）。
// C++ 端 protocol_test.cpp 使用同一文件交叉验证。

type vectors struct {
	SessionKey       string   `json:"session_key"`
	Sealed           []string `json:"sealed"`
	PlaintextHex     []string `json:"plaintext_hex"`
	Psk              string   `json:"psk"`
	ClientNonce      string   `json:"client_nonce"`
	ServerNonce      string   `json:"server_nonce"`
	EncKey           string   `json:"enc_key"`
	EncNonce         string   `json:"enc_nonce"`
	HeaderExampleHex string   `json:"header_example_hex"`
}

func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatalf("bad hex: %v", err)
	}
	return b
}

func loadVectors(t *testing.T) vectors {
	t.Helper()
	data, err := os.ReadFile(filepath.Join("testdata", "vectors.json"))
	if err != nil {
		t.Fatalf("read vectors: %v", err)
	}
	var v vectors
	if err := json.Unmarshal(data, &v); err != nil {
		t.Fatalf("parse vectors: %v", err)
	}
	return v
}

func TestDeriveSessionKey(t *testing.T) {
	v := loadVectors(t)
	got := DeriveSessionKey(mustHex(t, v.Psk), mustHex(t, v.ClientNonce), mustHex(t, v.ServerNonce))
	want := mustHex(t, v.SessionKey)
	if !bytes.Equal(got, want) {
		t.Fatalf("session key mismatch\n got: %s\nwant: %s", hex.EncodeToString(got), hex.EncodeToString(want))
	}
}

func TestSealMatchesVectors(t *testing.T) {
	// 用向量中的固定 key/nonce 手工 GCM 密封，必须等于向量密文
	v := loadVectors(t)
	key := mustHex(t, v.EncKey)
	nonce := mustHex(t, v.EncNonce)

	block, err := aes.NewCipher(key)
	if err != nil {
		t.Fatal(err)
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		t.Fatal(err)
	}

	for i, wantHex := range v.Sealed {
		pt := mustHex(t, v.PlaintextHex[i])
		sealed := gcm.Seal(nil, nonce, pt, nil)
		got := append(append([]byte{}, nonce...), sealed...)
		if hex.EncodeToString(got) != wantHex {
			t.Fatalf("seal[%d] mismatch\n got: %s\nwant: %s", i, hex.EncodeToString(got), wantHex)
		}
	}
}

func TestEncryptDecryptRoundtrip(t *testing.T) {
	key := make([]byte, SessionKeyLen)
	for i := range key {
		key[i] = byte(i)
	}
	msg := []byte("roundtrip payload 123")
	sealed, err := EncryptPayload(key, msg)
	if err != nil {
		t.Fatal(err)
	}
	// 密文段 = nonce(12) + ct + tag(16)
	if len(sealed) != NonceLen+len(msg)+16 {
		t.Fatalf("bad sealed length: %d", len(sealed))
	}
	got, err := DecryptPayload(key, sealed)
	if err != nil {
		t.Fatalf("decrypt: %v", err)
	}
	if !bytes.Equal(got, msg) {
		t.Fatal("roundtrip mismatch")
	}
}

func TestDecryptTamperFails(t *testing.T) {
	key := make([]byte, SessionKeyLen)
	sealed, err := EncryptPayload(key, []byte("tamper me"))
	if err != nil {
		t.Fatal(err)
	}
	sealed[len(sealed)-1] ^= 0xFF // 破坏 tag
	if _, err := DecryptPayload(key, sealed); err == nil {
		t.Fatal("tampered payload should fail GCM auth")
	}
}

func TestHeaderRoundtrip(t *testing.T) {
	h := &Header{
		Magic:      Magic,
		Version:    Version,
		Flags:      FlagEncrypted,
		Seq:        42,
		Timestamp:  123456,
		Cmd:        CmdHello,
		Plugin:     PluginCore,
		Status:     StatusOK,
		PayloadLen: 8,
	}
	b := EncodeHeader(h)
	if len(b) != HeaderSize {
		t.Fatalf("header size %d", len(b))
	}
	got, err := DecodeHeader(b)
	if err != nil {
		t.Fatal(err)
	}
	if *got != *h {
		t.Fatalf("header mismatch: %+v != %+v", got, h)
	}
}

func TestHeaderMatchesVectorExample(t *testing.T) {
	v := loadVectors(t)
	// header_example_hex: magic(5a5a) ver(01) flags(01) seq=1 ts=0 cmd=0101 plugin=00 status=00 len=0
	want := mustHex(t, v.HeaderExampleHex)
	h := &Header{
		Flags:   FlagEncrypted,
		Seq:     1,
		Cmd:     CmdHello,
		Plugin:  PluginCore,
		Status:  StatusOK,
	}
	got := EncodeHeader(h)
	if !bytes.Equal(got, want) {
		t.Fatalf("header example mismatch\n got: %s\nwant: %s", hex.EncodeToString(got), hex.EncodeToString(want))
	}
}
