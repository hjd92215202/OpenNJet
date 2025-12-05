local lor = require("lor.index")
local cjson = require("cjson")

local endUserRouter = lor:Router()

local tokenLib = require("njt.token")
local config = require("api_gateway.config.config")


local RETURN_CODE = {
    SUCCESS = 0,
    UUID_QUERY_FAIL = 10
}

local function getEndUserById(req, res, next)
    njt.log(njt.ERR, "=================call getuser:".. req.params.id)
    local retObj = {}
    local token_key = "param_end_user_id_uuid_" .. req.params.id
    local rc, tv_str = tokenLib.token_get(token_key)
    njt.log(njt.ERR, "=================1")
    if rc ~= 0 or not tv_str or tv_str == "" then
        retObj.code = RETURN_CODE.UUID_QUERY_FAIL
        retObj.msg = "user_uuid is not exist"
    else
        local userObj = {}
        userObj.user_uuid = tv_str

        retObj.code = RETURN_CODE.SUCCESS
        retObj.msg = "success"
        retObj.data = userObj
    end

    res:json(retObj, true)
end


local function putEndUserById(req, res, next)
    njt.log(njt.ERR, "=================call putuser: ".. req.params.id)
    local retObj = {}

    local ok, decodedObj = pcall(cjson.decode, req.body_raw)
    if not ok then
        retObj.code = RETURN_CODE.WRONG_PUT_DATA
        retObj.msg = "put data is not a valid json"
    else
        if not decodedObj.user_uuid then
            retObj.code = RETURN_CODE.WRONG_PUT_DATA
            retObj.msg = "put data is not a valid json"
        else
            -- set token
            local token_key = "param_end_user_id_uuid_" .. req.params.id
            local rc, msg = tokenLib.token_set(token_key, decodedObj.user_uuid, config.token_lifetime)
            if rc == 0 then
                retObj.code = RETURN_CODE.SUCCESS
                retObj.msg = "success"
            else
                retObj.code = RETURN_CODE.TOKEN_SET_FAIL
                retObj.msg = msg
            end
        end
    end

    res:json(retObj, true)
end



endUserRouter:put("/end_user/:id", putEndUserById)
endUserRouter:get("/end_user/:id", getEndUserById)


return endUserRouter
