local skynet = require "skynet"

skynet.start(function()
	print("Server start")
	--组播 group_mgr是每个服务器集群只有一个（即多个进程共有一个）。
	local service = skynet.newservice("service_mgr")
	skynet.monitor "simplemonitor"
	local lualog = skynet.newservice("lualog")
	local console = skynet.newservice("console")
	local remoteroot = skynet.newservice("remote_root")
	local watchdog = skynet.newservice("watchdog","8888 4 0")
	local db = skynet.newservice("simpledb")
	-- skynet.launch("snlua","testgroup")

	skynet.exit()
end)

--[[


 --]]
