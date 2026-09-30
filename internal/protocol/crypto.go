// Package protocol 实现 C2 协议规范 v1（见 docs/protocol.md）：
// 20 字节固定头、AES-256-GCM 加解密、HKDF-SHA256 会话密钥派生。
package protocol

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"errors"
	"io"
)

const (
	PSKLen         = 32 // 预共享密钥字节数
	SessionKeyLen  = 32 // 派生会话密钥字节数
	NonceLen       = 12 // AES-GCM 每包 nonce 字节数
	ClientNonceLen = 16 // 握手客户端随机数
	ServerNonceLen = 16 // 握手服务端随机数
)

// hkdfSha256 实现 RFC 5869 HKDF（SHA-256），避免外部依赖。
func hkdfSha256(ikm, salt, info []byte, outLen int) []byte {
	// Extract: PRK = HMAC-SHA256(salt, ikm)
	h := hmac.New(sha256.New, salt)
	h.Write(ikm)
	prk := h.Sum(nil)

	// Expand: T(i) = HMAC-SHA256(PRK, T(i-1) || info || i)
	var out, t []byte
	for counter := byte(1); len(out) < outLen; counter++ {
		h := hmac.New(sha256.New, prk)
		h.Write(t)
		h.Write(info)
		h.Write([]byte{counter})
		t = h.Sum(nil)
		out = append(out, t...)
	}
	return out[:outLen]
}

// DeriveSessionKey 按协议 §4：HKDF-SHA256(PSK, client_nonce||server_nonce, "rat-v1", 32)
func DeriveSessionKey(psk, clientNonce, serverNonce []byte) []byte {
	salt := make([]byte, 0, len(clientNonce)+len(serverNonce))
	salt = append(salt, clientNonce...)
	salt = append(salt, serverNonce...)
	return hkdfSha256(psk, salt, []byte("rat-v1"), SessionKeyLen)
}

// BeaconKey 按协议 §8.2：beacon_key = HKDF-SHA256(PSK, agent_id, "rat-beacon-v1", 32)
// 无状态派生：服务器从请求中的 agent_id 即可重算，无需握手/持久化。
func BeaconKey(psk, agentID []byte) []byte {
	return hkdfSha256(psk, agentID, []byte("rat-beacon-v1"), SessionKeyLen)
}

// StreamKey 按协议 §9.1：stream_key = HKDF-SHA256(PSK,
// agent_id||client_nonce||server_nonce, "rat-stream-v1", 32)
func StreamKey(psk, agentID, clientNonce, serverNonce []byte) []byte {
	salt := make([]byte, 0, len(agentID)+len(clientNonce)+len(serverNonce))
	salt = append(salt, agentID...)
	salt = append(salt, clientNonce...)
	salt = append(salt, serverNonce...)
	return hkdfSha256(psk, salt, []byte("rat-stream-v1"), SessionKeyLen)
}

// EncryptPayload 返回 nonce(12) || ciphertext || tag(16)
func EncryptPayload(key, plaintext []byte) ([]byte, error) {
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return nil, err
	}
	nonce := make([]byte, gcm.NonceSize())
	if _, err := io.ReadFull(rand.Reader, nonce); err != nil {
		return nil, err
	}
	return gcm.Seal(nonce, nonce, plaintext, nil), nil
}

// DecryptPayload 解密 EncryptPayload 的输出；tag 校验失败返回错误。
func DecryptPayload(key, sealed []byte) ([]byte, error) {
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return nil, err
	}
	if len(sealed) < gcm.NonceSize()+gcm.Overhead() {
		return nil, errors.New("sealed payload too short")
	}
	return gcm.Open(nil, sealed[:gcm.NonceSize()], sealed[gcm.NonceSize():], nil)
}
