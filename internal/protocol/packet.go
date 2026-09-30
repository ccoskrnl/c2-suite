package protocol

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

// ---- 协议常量（docs/protocol.md）----
const (
	Magic      uint16 = 0x5A5A
	Version    uint8  = 1
	HeaderSize        = 20
	MaxPayload        = 16 * 1024 * 1024 // 16 MiB
)

const (
	FlagEncrypted uint8 = 0x01
	FlagCompress  uint8 = 0x02 // 预留
)

// 插件 ID（头部 plugin 字段）
const (
	PluginCore   uint8 = 0x00
	PluginRemote uint8 = 0x01
	PluginFile   uint8 = 0x02
	PluginSys    uint8 = 0x03
	PluginCtl    uint8 = 0xFF
)

// 连接角色（HELLO 内）
const (
	RoleAgent   uint8 = 0
	RoleControl uint8 = 1
)

// 核心指令码
const (
	CmdHello       uint16 = 0x0101
	CmdHelloAck    uint16 = 0x0102
	CmdHeartbeat   uint16 = 0x0103
	CmdResult      uint16 = 0x0104
	CmdEvent       uint16 = 0x0105
	CmdBye         uint16 = 0x0106
	CtlList        uint16 = 0x4001
	CtlKillSession uint16 = 0x4002
	CtlGetSession  uint16 = 0x4003
)

// 业务指令码（系统段 0x3xxx）
const (
	CmdSysInfo uint16 = 0x3001 // 收集系统信息（plugin=PluginSys）
	CmdExecPs  uint16 = 0x3008 // 执行 PowerShell 命令（plugin=PluginSys）
	CmdPayload uint16 = 0x3009 // payload 分块上传并执行（plugin=PluginSys，协议 §10）
)

// 业务指令码（远程控制段 0x1xxx，plugin=PluginRemote）
const (
	CmdScreenshot   uint16 = 0x1001 // 单帧截屏（响应：JPEG 原始字节）
	CmdScreenStream uint16 = 0x1002 // 屏幕流控制（beacon 信道：agent_id + 控制/fps/quality）
)

// payload 分块上传参数（协议 §10，两端契约）
const (
	PayloadChunkSize = 256 * 1024      // 单块数据上限 256 KiB
	PayloadMaxTotal  = 64 * 1024 * 1024 // 落盘总大小上限 64 MiB
	PayloadMaxNameLen = 128
	PayloadFlagLast   = 0x01
)

// 屏幕流长连接段（0x5xxx，走专用 TCP 长连接 4445，不经 beacon）
const (
	CmdStreamHello     uint16 = 0x0501 // 屏幕连接握手（明文）：agent_id(16)+client_nonce(16)
	CmdStreamAck       uint16 = 0x0502 // 握手应答（明文）：server_nonce(16)
	CmdScreenFrame     uint16 = 0x0503 // 帧包（加密）：seq(4)+w(2)+h(2)+jpeg_len(4)+jpeg
	CmdInputEvents     uint16 = 0x0504 // 输入事件（加密）：count(4)+InputEvent[]
)

// 状态码
const (
	StatusOK            uint8 = 0
	StatusUnknownCmd    uint8 = 1
	StatusBadParam      uint8 = 2
	StatusNoPermission  uint8 = 3
	StatusNotFound      uint8 = 4
	StatusTimeout       uint8 = 5
	StatusNoSession     uint8 = 6
	StatusPluginErrBase uint8 = 0x80
)

// ---- 头部 ----

// Header 是 20 字节固定头部（little-endian）。
type Header struct {
	Magic     uint16
	Version   uint8
	Flags     uint8
	Seq       uint32
	Timestamp uint32
	Cmd       uint16
	Plugin    uint8
	Status    uint8
	PayloadLen uint32
}

// EncodeHeader 将头部编码为 20 字节；magic/version 由本函数强制写入。
func EncodeHeader(h *Header) []byte {
	h.Magic = Magic
	h.Version = Version
	b := make([]byte, HeaderSize)
	binary.LittleEndian.PutUint16(b[0:], h.Magic)
	b[2] = h.Version
	b[3] = h.Flags
	binary.LittleEndian.PutUint32(b[4:], h.Seq)
	binary.LittleEndian.PutUint32(b[8:], h.Timestamp)
	binary.LittleEndian.PutUint16(b[12:], h.Cmd)
	b[14] = h.Plugin
	b[15] = h.Status
	binary.LittleEndian.PutUint32(b[16:], h.PayloadLen)
	return b
}

// DecodeHeader 解析 20 字节头部并校验 magic/version。
func DecodeHeader(b []byte) (*Header, error) {
	if len(b) < HeaderSize {
		return nil, errors.New("header too short")
	}
	h := &Header{
		Magic:      binary.LittleEndian.Uint16(b[0:]),
		Version:    b[2],
		Flags:      b[3],
		Seq:        binary.LittleEndian.Uint32(b[4:]),
		Timestamp:  binary.LittleEndian.Uint32(b[8:]),
		Cmd:        binary.LittleEndian.Uint16(b[12:]),
		Plugin:     b[14],
		Status:     b[15],
		PayloadLen: binary.LittleEndian.Uint32(b[16:]),
	}
	if h.Magic != Magic {
		return nil, fmt.Errorf("bad magic 0x%04x", h.Magic)
	}
	if h.Version != Version {
		return nil, fmt.Errorf("bad version %d", h.Version)
	}
	if h.PayloadLen > MaxPayload {
		return nil, fmt.Errorf("payload too large: %d", h.PayloadLen)
	}
	return h, nil
}

// ---- 分帧 ----

// ReadPacket 从 r 读取一包：20B 头部 + payload。
// 返回 Header 与原始 payload（未解密）；连接关闭返回 io.EOF 之外错误为协议错误。
func ReadPacket(r io.Reader) (*Header, []byte, error) {
	hdrBuf := make([]byte, HeaderSize)
	if _, err := io.ReadFull(r, hdrBuf); err != nil {
		return nil, nil, err
	}
	h, err := DecodeHeader(hdrBuf)
	if err != nil {
		return nil, nil, err
	}
	payload := make([]byte, h.PayloadLen)
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, nil, err
	}
	return h, payload, nil
}

// WritePacket 向 w 写一包（头部 + payload，合并为一次 Write）。
func WritePacket(w io.Writer, h *Header, payload []byte) error {
	if len(payload) > MaxPayload {
		return errors.New("payload too large")
	}
	h.Magic = Magic
	h.Version = Version
	h.PayloadLen = uint32(len(payload))
	buf := make([]byte, 0, HeaderSize+len(payload))
	buf = append(buf, EncodeHeader(h)...)
	buf = append(buf, payload...)
	if _, err := w.Write(buf); err != nil {
		return err
	}
	return nil
}
