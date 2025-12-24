local userDao = require("api_gateway.dao.user")
local groupDao = require("api_gateway.dao.group")
local util = require("api_gateway.utils.util")
local authDao = require("api_gateway.dao.auth")
local tokenLib=require("njt.token")
local sysConfigDao = require("api_gateway.dao.sys_config")

local _M = {}

local function get_user_with_username_password(login_data)
    -- if password is empty, return 
    if not login_data.password then
        return false, nil
    end

    local encryptedPassword = util.encryptPassword(login_data.password)
    return userDao.getUserByNameAndPassword(login_data.username, encryptedPassword)
end

local function get_user_with_email(login_data)
    -- if verification_code is empty, return 
    if not login_data.verification_code then
        return false, nil
    end

    -- validate verification_code
    -- local ok, retObj = authDao.getVerificationCode(login_data.verification_code)
    local rc, token=tokenLib.token_get("sms_login_token_"..login_data.email)
    njt.log(njt.ERR, "sms token is: ".. tostring(token))
    if rc ~= 0 or not token or token ~= login_data.verification_code then 
        return false, "verification code is not valid"
    end

    return userDao.getUserByEmail(login_data.email)
end

local function get_token_and_userinfo_from_keycloak(login_data)
    local http = require "resty.http"
    local cjson = require "cjson"

    local keycloak_username = login_data.username:match("([^@]+)") or login_data.username

    -- get keycloak config from db
    local keycloak_client_id = nil
    local keycloak_client_secret = nil
    local keycloak_scope = nil
    local keycloak_grant_type = nil
    local keycloak_token_url = nil
    local keycloak_userinfo_url = nil

    local ok, configs = sysConfigDao.getSysConfig()
    if ok and configs and #configs>0 then
        for _, config in ipairs(configs) do
            njt.log(njt.INFO, "get keycloak sys config key:".. tostring(config.config_key).. "  value:".. config.config_value)
            if config.config_key == "keycloak_client_id" then
                keycloak_client_id = config.config_value
            elseif config.config_key == "keycloak_client_secret" then
                keycloak_client_secret = tostring(njt.decode_base64(tostring(config.config_value)))
            elseif config.config_key == "keycloak_scope" then
                keycloak_scope = config.config_value
            elseif config.config_key == "keycloak_grant_type" then
                keycloak_grant_type = config.config_value
            elseif config.config_key == "keycloak_token_url" then
                keycloak_token_url = config.config_value
            elseif config.config_key == "keycloak_userinfo_url" then
                keycloak_userinfo_url = config.config_value
            end
        end
    end

    njt.log(njt.INFO, "get keycloak sys config keycloak_client_id:".. keycloak_client_id)
    njt.log(njt.INFO, "get keycloak sys config keycloak_client_secret:".. keycloak_client_secret)
    njt.log(njt.INFO, "get keycloak sys config keycloak_scope:".. tostring(keycloak_scope))
    njt.log(njt.INFO, "get keycloak sys config keycloak_grant_type:".. keycloak_grant_type)
    njt.log(njt.INFO, "get keycloak sys config keycloak_token_url:".. keycloak_token_url)
    njt.log(njt.INFO, "get keycloak sys config keycloak_userinfo_url:".. keycloak_userinfo_url)

    if not keycloak_client_id or not keycloak_client_secret or not keycloak_scope or not keycloak_grant_type
        or not keycloak_token_url or not keycloak_userinfo_url then
        njt.log(njt.INFO, "get keycloak sys config error, some is nil")
        return false, nil, nil
    end

    local body = {
        username = keycloak_username,
        password = login_data.password,
        client_id = keycloak_client_id,
        client_secret = keycloak_client_secret,
        scope = keycloak_scope,
        grant_type = keycloak_grant_type
    }

    local httpc = http.new()

    local res, err = httpc:request_uri(keycloak_token_url, {
        method = "POST",
        body = njt.encode_args(body),
        ssl_verify = false,
        headers = {
            ["Content-Type"] = "application/x-www-form-urlencoded"
        }
    })
    
    if not res then
        njt.log(njt.INFO, "get access token from keycloak error:", err)
        return false, nil, nil
    end
    
    njt.log(njt.INFO, "get keycloak token body:", res.body)

    local success, data = pcall(cjson.decode, res.body)
    if not success then
        njt.log(njt.ERR, "parse keyclaok token info error:", res.body)
        return false, nil, nil
    end

    local access_token = data["access_token"]

    if not access_token then
        njt.log(njt.ERR, "keyclaok access_token is not exist")
        return false, nil, nil
    end

    local auth_access_token = string.format("Bearer %s", access_token)
    local userinfo, err = httpc:request_uri(keycloak_userinfo_url, {
            method = "GET",
            ssl_verify = false,
            headers = {
                    ["Content-Type"] = "application/x-www-form-urlencoded",
                    ["Authorization"] = auth_access_token
            }
    })

    if not userinfo then
        njt.log(njt.INFO, "get keycloak user info error:", err)
        return false, nil, nil
    end

    njt.log(njt.INFO, "keycloak user info:", userinfo.body)

    local userinfo_ok, userinfo_data = pcall(cjson.decode, userinfo.body)
    if not userinfo_ok then
        njt.log(njt.ERR, "parse keyclaok userinfo info error:", userinfo.body)
        return false, nil, nil
    end

    return true, access_token, userinfo_data
end


function _M.login(login_data)
    if not login_data.username or not login_data.password then
        return false, nil, nil, nil
    end

    -- get token and user info from keycloak
    local ok, token, userinfo = get_token_and_userinfo_from_keycloak(login_data)
    if not ok then
        njt.log(njt.ERR, "get token and user info from keycloak error")
        return false, nil, nil, nil
    end

    -- check group info
    if not userinfo.groups or #userinfo.groups < 1 then
        njt.log(njt.ERR, "group info is empty of user:".. login_data.username)
        return false, nil, nil, nil
    end

    local inputObj = {}
    inputObj.name = login_data.username
    inputObj.password = login_data.password
    inputObj.email = userinfo["email"]
    inputObj.mobile = userinfo.mobile
    inputObj.password = util.encryptPassword(inputObj.password)
    inputObj.nickname = login_data.username

    -- check local wether has user
    local user_exist, userObj = userDao.getUserByName(inputObj.name)
    if not user_exist then
        -- if exists, then update local user info 
        local create_user_ok, userObj = userDao.createUser(inputObj)
        if not create_user_ok then
            njt.log(njt.ERR, "create user error when use keycloak userinfo")
            return false, nil, nil, nil
        end

        njt.log(njt.INFO, "create user success")
    else
        -- if not exists, then create local user info
        inputObj.id = userObj.id
        inputObj.email = userObj.email
        inputObj.mobile = userObj.mobile
        local update_user_ok, msg = userDao.updateUser(inputObj)
        if not update_user_ok then
            njt.log(njt.ERR, "udpate user error when use keycloak userinfo")
            return false, nil, nil, nil
        end
    end

    -- get domain from sys_config
    local keycloak_domain = login_data.username:match("@([^@]+)$")
    -- sync user group relation
    local group_item = nil
    for i, g in ipairs(userinfo.groups) do
        group_item = tostring(g) .. "@" .. tostring(keycloak_domain)
        -- format group_item@domain
        local group_exist, groupObj = groupDao.getGroupByName(group_item)
        -- if not exist group
        if not group_exist then
            -- add group
            local groupInputObj = {}
            groupInputObj.name = group_item
            groupInputObj.desc = "API Gateway ".. tostring(group_item) .. " group"
            local create_group_ok, groupObj = groupDao.createGroup(groupInputObj)
            if not create_group_ok then
                njt.log(njt.ERR, "create group error: ".. group_item)
                return false, nil, nil, nil
            end

            -- add group_role_relation(innner role of general, roleid is 2)
            local userGroupRoleObj = {id=groupObj.id,roles={}}
            table.insert(userGroupRoleObj.roles, 2)
            local update_grouprole_ok, msg = groupDao.updateUserGroupRoleRel(userGroupRoleObj)
            if not update_grouprole_ok then
                njt.log(njt.ERR, "update group role relation error: ".. group_item)
                return false, nil, nil, nil
            end
        end
    end

    local inputGroupObj = {}
    inputGroupObj.groups = {}
    -- get group id
    for i, g in ipairs(userinfo.groups) do
        group_item = tostring(g) .. "@" .. tostring(keycloak_domain)
        local get_group_ok, groupObj = groupDao.getGroupByName(group_item)
        if not get_group_ok then
            njt.log(njt.ERR, "get group error: ".. group_item)
            return false, nil, nil, nil
        end

        njt.log(njt.INFO, "get group name: ".. tostring(group_item))
        njt.log(njt.INFO, "get group info: ".. tostring(groupObj))
        table.insert(inputGroupObj.groups, groupObj.id)
    end

    inputGroupObj.id = userObj.id

    local update_group_ok, msg = userDao.updateUserGroupRel(inputGroupObj)
    if not update_group_ok then
        userDao.deleteUserById(userObj.id)
        njt.log(njt.ERR, "udpate user group error")
        return false, nil, nil, nil
    end

    njt.log(njt.INFO, "update user group success")

    ok, userObj = get_user_with_username_password(login_data)
    if not ok then
        return false, nil, userObj, nil
    end

    njt.log(njt.INFO, "login success")
    local keycloakInfo = {}
    keycloakInfo.token = token
    keycloakInfo.uuid = userinfo.sub

    return true, userObj.id, userObj, keycloakInfo
end

return _M
