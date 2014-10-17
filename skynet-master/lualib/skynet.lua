local c = require "skynet.c"
local mc = require "mcast.c"
local tostring = tostring
local tonumber = tonumber
local coroutine = coroutine
local assert = assert
local pairs = pairs
local pcall = pcall

local proto = {}
local skynet = {}

-- 注册一个协议
function skynet.register_protocol(class)
	local name = class.name
	local id = class.id
	assert(proto[name] == nil)
	assert(type(name) == "string" and type(id) == "number" and id >=0 and id <=255)
	proto[name] = class
	proto[id] = class
end

local session_id_coroutine = {}
local session_coroutine_id = {}
local session_coroutine_address = {}

local wakeup_session = {}
local sleep_session = {}

local trace_handle
local trace_func = function() end

-- coroutine reuse

local coroutine_pool = {}
local coroutine_yield = coroutine.yield

local function co_create(f)
	local co = table.remove(coroutine_pool)
	if co == nil then
		co = coroutine.create(function(...)
			f(...)
			while true do
				f = nil
				coroutine_pool[#coroutine_pool] = co
				f = coroutine_yield "EXIT"
				f(coroutine_yield())
			end
		end)
	else
		coroutine.resume(co, f)
	end
	return co
end

-- suspend is function
local suspend

local function dispatch_wakeup()
	local co = next(wakeup_session)
	if co then
		wakeup_session[co] = nil
		local session = sleep_session[co]
		if session then
			session_id_coroutine[session] = "BREAK"
			return suspend(co, coroutine.resume(co, true))
		end
	end
end

local function trace_count()
	local info = c.trace_yield(trace_handle)
	if info then
		local ti = c.trace_delete(trace_handle, info)
		trace_func(info, ti)
	end
end

-- suspend is local function
function suspend(co, result, command, param, size)
	-- result是执行协程的结果，若成功返回true，否则返回false
	if not result then
		trace_count()
		error(debug.traceback(co,command))
	end
	if command == "CALL" then
		c.trace_register(trace_handle, param)
		session_id_coroutine[param] = co
	elseif command == "SLEEP" then
		c.trace_register(trace_handle, param)
		session_id_coroutine[param] = co
		sleep_session[co] = param
	elseif command == "RETURN" then
		local co_session = session_coroutine_id[co]
		local co_address = session_coroutine_address[co]
		-- PTYPE_RESPONSE = 1 , see skynet.h
		if param == nil then
			trace_count()
			error(debug.traceback(co))
		end
		-- c.send maybe throw a error, so call trace_count first.
		-- The coroutine execute time after skynet.ret() will not be trace.
		trace_count()
		c.send(co_address, 1, co_session, param, size)
		return suspend(co, coroutine.resume(co))
	elseif command == "EXIT" then
		-- coroutine exit
		session_coroutine_id[co] = nil
		session_coroutine_address[co] = nil
	else
		trace_count()
		error("Unknown command : " .. command .. "\n" .. debug.traceback(co))
	end
	trace_count()
	dispatch_wakeup()
end

function skynet.timeout(ti, func)
	local session = c.command("TIMEOUT",tostring(ti))
	assert(session)
	session = tonumber(session)
	local co = co_create(func)
	assert(session_id_coroutine[session] == nil)
	session_id_coroutine[session] = co
end

function skynet.sleep(ti)
	local session = c.command("TIMEOUT",tostring(ti))
	assert(session)
	session = tonumber(session)
	local ret = coroutine_yield("SLEEP", session)
	sleep_session[coroutine.running()] = nil
	if ret == true then
		c.trace_switch(trace_handle, session)
		return "BREAK"
	end
end

function skynet.yield()
	return skynet.sleep("0")
end

function skynet.wait()
	local session = c.genid()
	coroutine_yield("SLEEP", session)
	c.trace_switch(trace_handle, session)
	local co = coroutine.running()
	sleep_session[co] = nil
	session_id_coroutine[session] = nil
end

-- 向skynet 注册一个别名
function skynet.register(name)
	c.command("REG", name)
end

function skynet.name(name, handle)
	c.command("NAME", name .. " " .. handle)
end

local function string_to_handle(str)
	-- 将十六进制handle转换成十进制 eg. ":1000008"
	return tonumber("0x" .. string.sub(str , 2))
end

-- 获取自己上下文 注 每一个服务都是独立的
local self_handle
function skynet.self()
	if self_handle then
		return self_handle
	end
	-- 调用c的command方法发送reg命令，并获取自己的handle
	self_handle = string_to_handle(c.command("REG"))
	return self_handle
end

function skynet.localname(name)
	return string_to_handle(c.command("QUERY", name))
end

-- 启动一项 服务
function skynet.launch(...)
	-- print("lua...skynet.launch : " .. table.concat({...} , " "));
	local addr = c.command("LAUNCH", table.concat({...}," "))
	if addr then
		return string_to_handle(addr)
	end
end

function skynet.now()
	return tonumber(c.command("NOW"))
end

function skynet.starttime()
	return tonumber(c.command("STARTTIME"))
end

function skynet.exit()
	skynet.send(".launcher","lua","REMOVE",skynet.self())
	c.command("EXIT")
end

function skynet.kill(name)
	if type(name) == "number" then
		skynet.send(".launcher","lua","REMOVE",name)
		name = skynet.address(name)
	end
	c.command("KILL",name)
end

function skynet.getenv(key)
	return c.command("GETENV",key)
end

function skynet.setenv(key, value)
	c.command("SETENV",key .. " " ..value)
end

function skynet.send(addr, typename, ...)
	local p = proto[typename]
	return c.send(addr, p.id, 0 , p.pack(...))
end

function skynet.cast(group, typename, ...)
	local p = proto[typename]
	if #group > 0 then
		return c.send(".cast", p.id, 0, mc(group, p.pack(...)))
	end
end

skynet.genid = assert(c.genid)
skynet.forward = assert(c.forward)

skynet.redirect = function(dest,source,typename,...)
	return c.redirect(dest, source, proto[typename].id, ...)
end

skynet.pack = assert(c.pack)
skynet.unpack = assert(c.unpack)
skynet.tostring = assert(c.tostring)

function skynet.call(addr, typename, ...)
	local p = proto[typename]
	local session = c.send(addr, p.id , nil , p.pack(...))
	return p.unpack(coroutine_yield("CALL", assert(session, "call to invalid address")))
end

function skynet.blockcall(addr, typename , ...)
	local p = proto[typename]
	c.command("LOCK")
	local session = c.send(addr, p.id , nil , p.pack(...))
	if session == nil then
		c.command("UNLOCK")
		error("call to invalid address")
	end
	return p.unpack(coroutine_yield("CALL", session))
end

function skynet.rawcall(addr, typename, msg, sz)
	local p = proto[typename]
	local session = c.send(addr, p.id , nil , msg, sz)
	return coroutine_yield("CALL", assert(session, "call to invalid address"))
end

function skynet.ret(msg, sz)
	msg = msg or ""
	-- 结束协程，回到主线程
	coroutine_yield("RETURN", msg, sz)
end

function skynet.wakeup(co)
	if sleep_session[co] and wakeup_session[co] == nil then
		wakeup_session[co] = true
		return true
	end
end

function skynet.dispatch(typename, func)
	local p = assert(proto[typename],tostring(typename))
	assert(p.dispatch == nil, tostring(typename))
	p.dispatch = func
end

local function unknown_response(session, address, msg, sz)
	error(string.format("Unknown session : %d from %x", session, address))
end

function skynet.dispatch_unknown_response(unknown)
	local prev = unknown_response
	unknown_response = unknown
	return prev
end

local fork_queue = {}

function skynet.fork(func,...)
	local args = { ... }
	local co = co_create(function()
		func(unpack(args))
	end)
	table.insert(fork_queue, co)
end

local function raw_dispatch_message(prototype, msg, sz, session, source, ...)
	-- PTYPE_RESPONSE = 1, read skynet.h
	if prototype == 1 then
		-- 回应包
		local co = session_id_coroutine[session]
		if co == "BREAK" then
			session_id_coroutine[session] = nil
		elseif co == nil then
			unknown_response(session, source, msg, sz)
		else
			c.trace_switch(trace_handle, session)
			session_id_coroutine[session] = nil
			-- 执行分发协程
			suspend(co, coroutine.resume(co, msg, sz))
		end
	else
		-- 发送包
		local p = assert(proto[prototype], prototype)
		local f = p.dispatch
		if f then
			local co = co_create(f)
			session_coroutine_id[co] = session
			session_coroutine_address[co] = source
			-- 执行分发协程
			suspend(co, coroutine.resume(co, session,source, p.unpack(msg,sz, ...)))
		else
			print("Unknown request :" , p.unpack(msg,sz))
			error(string.format("Can't dispatch type %s : ", p.name))
		end
	end
end

local function dispatch_message(...)
	-- print("local function dispatch_message(...)....");
	-- 通过pcall调用了接口raw_dispatch_message
	local succ, err = pcall(raw_dispatch_message,...)
	while true do
		local key,co = next(fork_queue)
		if co == nil then
			break
		end
		fork_queue[key] = nil
		local fork_succ, fork_err = pcall(suspend,co,coroutine.resume(co))
		if not fork_succ then
			if succ then
				succ = false
				err = fork_err
			else
				err = err .. "\n" .. fork_err
			end
		end
	end
	assert(succ, err)
end

function skynet.newservice(name, ...)
	local param =  table.concat({"snlua", name, ...}, " ")
	local handle = skynet.call(".launcher", "text" , param)
	if handle == "" then
		return nil
	else
		return string_to_handle(handle)
	end
end

function skynet.uniqueservice(global, ...)
	local t = 0
	while true do
		local handle
		if global == true then
			handle = skynet.call("SERVICE", "lua", ...)
		else
			handle = skynet.call(".service", "lua", global, ...)
		end
		if handle ~= false then
			return handle
		end
		skynet.sleep(t)
		t=t+1
	end
end

local function group_command(cmd, handle, address)
	if address then
		return string.format("%s %d :%x",cmd, handle, address)
	else
		return string.format("%s %d",cmd,handle)
	end
end

function skynet.enter_group(handle , address)
	c.command("GROUP", group_command("ENTER", handle, address))
end

function skynet.leave_group(handle , address)
	c.command("GROUP", group_command("LEAVE", handle, address))
end

function skynet.clear_group(handle)
	c.command("GROUP", "CLEAR " .. tostring(handle))
end

function skynet.query_group(handle)
	return string_to_handle(c.command("GROUP","QUERY " .. tostring(handle)))
end

function skynet.address(addr)
	return string.format(":%x",addr)
end

function skynet.harbor(addr)
	return c.harbor(addr)
end

------ remote object --------

do
	-- proto is 11

	local remote_query, remote_alloc, remote_bind = c.remote_init(skynet.self())
	local weak_meta = { __mode = "kv" }
	local meta = getmetatable(c.unpack(c.pack({ __remote = 0 })))
	local remote_call_func = setmetatable({}, weak_meta)

	local _send = assert(c.send)
	local _yield = coroutine.yield
	local _pack = assert(c.pack)
	local _unpack = assert(c.unpack)
	local _local = skynet.self()

	function meta__index(t, method)
		local f = remote_call_func[method]
		if f == nil then
			f = function(...)
				local addr = remote_query(t.__remote)
				-- the proto is 11 (lua is 10)
				local session = assert(_send(addr, 11 , nil, _pack(t,method,...)), "call to invalid address")
				local msg, sz = _yield("CALL", session)
				return select(2,assert(_unpack(msg,sz)))
			end
			remote_call_func[method] = f
		end
		rawset(t,method,f)
		return f
	end

	-- prevent gc
	meta.__index = meta__index

	meta.__newindex = error

	function skynet.remote_create(t, handle)
		t = t or {}
		if handle then
			remote_bind(handle)
		else
			handle = remote_alloc()
		end
		rawset(t, "__remote" , handle)
		rawset(meta, handle, t)
		return t
	end

	function skynet.remote_bind(handle)
		return setmetatable( { __remote = handle } , meta)
	end

	local function remote_call(obj, method, ...)
		if type(obj) ~= "table" or type(method) ~= "string" then
			return _yield("RETURN", _pack(false, "Invalid call"))
		end
		local f = obj[method]
		if type(f) ~= "function" then
			return _yield("RETURN", _pack(false, "Object has not method " .. method))
		end
		return _yield("RETURN", _pack(pcall(f,...)))
	end

	function skynet.remote_root()
		return skynet.remote_bind(0)
	end

	skynet.register_protocol {
		name = "remoteobj",
		id = 11,
		unpack = c.unpack,
		dispatch = function (session, source, ...)
			remote_call(...)
		end
	}
end

----- debug

local internal_info_func

function skynet.info_func(func)
	internal_info_func = func
end

local dbgcmd = {}

function dbgcmd.MEM()
	local kb, bytes = collectgarbage "count"
	skynet.ret(skynet.pack(kb,bytes))
end

function dbgcmd.GC()
	coroutine_pool = {}
	collectgarbage "collect"
end

local function query_state(stat, what)
	stat[what] = c.stat(what)
end

function dbgcmd.STAT()
	local stat = {}
	query_state(stat, "count")
	query_state(stat, "time")
	skynet.ret(skynet.pack(stat))
end

function dbgcmd.INFO()
	if internal_info_func then
		skynet.ret(skynet.pack(internal_info_func()))
	else
		skynet.ret(skynet.pack(nil))
	end
end

function dbgcmd.RELOAD(...)
	local cmd = table.concat({...}, " ")
	c.reload(cmd)
end

local function _debug_dispatch(session, address, cmd, ...)
	local f = dbgcmd[cmd]
	assert(f, cmd)
	f(...)
end

----- register protocol
do
	-- skynet.register_protocol注册一个客户端消息处理协议

	-- 每个服务可以使用不同的协议组，则是在底层由 type 参数区分的。
	-- 在 lua 层，可以为每个不同的 type 编写不同的 dispatch 函数。
	-- 默认仅提供了 RESPONSE 消息的处理方法，每个独立的 lua 服务，
	-- 都需要去实现自己可以支持的协议类型的处理函数。
	local REG = skynet.register_protocol
	-- 这些协议对应skynet.h中定义的塄以
	-- #define PTYPE_TEXT 0  text协议
	REG {
		name = "text",
		id = 0,
		pack = function (...)
			local n = select ("#" , ...)
			if n == 0 then
				return ""
			elseif n == 1 then
				return tostring(...)
			else
				return table.concat({...}," ")
			end
		end,
		unpack = c.tostring
	}

	-- #define PTYPE_RESERVED_LUA 10  lua协议
	-- 我称为 lua 协议。因为它仅仅是简单的把 Lua 支持的类型系列化起来，
	-- 另一个 Lua 服务可以顺利的解开它们。这样，看起来请求（用 Lua 编写的）远程服务和调用本地函数一样简单。
	REG {
		name = "lua",
		id = 10,
		pack = skynet.pack,
		unpack = skynet.unpack,
	}

	-- #define PTYPE_RESPONSE 1    返回包
	REG {
		name = "response",
		id = 1,
	}

	-- #define PTYPE_RESERVED_DEBUG 9
	REG {
		name = "debug",
		id = 9,
		pack = skynet.pack,
		unpack = skynet.unpack,
		dispatch = _debug_dispatch,
	}
end

local init_func = {}

function skynet.init(f, name)
	assert(type(f) == "function")
	if init_func == nil then
		f()
	else
		if name == nil then
			table.insert(init_func, f)
		else
			assert(init_func[name] == nil)
			init_func[name] = f
		end
	end
end

local function init_all()
	local funcs = init_func
	init_func = nil
	for k,v in pairs(funcs) do
		v()
	end
end

local function init_template(start)
	init_all()
	init_func = {}
	start()
	init_all()
end

function skynet.start(start_func)
	c.callback(dispatch_message)
	trace_handle = assert(c.stat "trace")
	skynet.timeout(0, function()
		init_template(start_func)
		skynet.send(".launcher","text", "")
	end)
end

function skynet.filter(f ,start_func)
	c.callback(function(...)
		dispatch_message(f(...))
	end)
	trace_handle = assert(c.stat "trace")
	skynet.timeout(0, function()
	init_template(start_func)
		skynet.send(".launcher","text", "")
	end)
end

function skynet.trace()
	return c.trace_new(trace_handle)
end

function skynet.trace_session(session)
	return c.trace_register(trace_handle, session)
end

function skynet.trace_callback(func)
	trace_func = func
end

function skynet.endless()
	return c.command("ENDLESS")~=nil
end

function skynet.abort()
	c.command("ABORT")
end

function skynet.context_ptr()
	return c.context()
end

function skynet.monitor(service)
	local monitor = skynet.uniqueservice(true, service)
	c.command("MONITOR", string.format(":%08x", monitor))
end

return skynet
