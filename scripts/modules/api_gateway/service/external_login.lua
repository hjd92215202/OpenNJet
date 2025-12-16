local userDao = require("api_gateway.dao.user")
local groupDao = require("api_gateway.dao.group")
local util = require("api_gateway.utils.util")
local authDao = require("api_gateway.dao.auth")
local tokenLib=require("njt.token")

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


    -- get keycloak config from db
    

    local body = {
        username = login_data.username,
        password = login_data.password,
        client_id = 'bigModel',
        client_secret = 'gx9ycS7j1oHLP9AKsqPCZRrdV7F4rrXU',
        scope = 'openid,groups',
        grant_type = 'password'
    }

    local httpc = http.new()

    local res, err = httpc:request_uri("https://192.168.40.73:8443/realms/tmlake/protocol/openid-connect/token", {
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
    
    -- njt.log(njt.INFO, "get keycloak token body:", res.body)

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
    local userinfo, err = httpc:request_uri("https://192.168.40.73:8443/realms/tmlake/protocol/openid-connect/userinfo", {
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

    -- 

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
    if not userinfo_data.groups or #userinfo_data.groups < 1 then
        njt.log(njt.ERR, "group info is empty of user:".. login_data.username)
        return false, nil, nil, nil
    end

    local inputObj = {}
    -- inputObj.domain = "tmlake.com"
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
        -- need update group info of user

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
    local keycloak_domain = nil

    -- sync user group relation
    local group_item = nil
    for i, g in ipairs(userinfo_data.groups) do
        group_item = tostring(g)
        -- format group_item@domain
        group_item = group_item.. "@".. keycloak_domain
        njt.log(njt.INFO, "Group[", i, "] = ", group_item)
        local group_exist, groupObj = groupDao.getGroupByName(group_item)
        
        -- if not exist group
        if not group_exist then
            -- add group
            local groupInputObj = {}
            groupInputObj.name = group_item
            groupInputObj.desc = "API Gateway ".. group_item.. " group"
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
    for i, g in ipairs(userinfo_data.groups) do
        group_item = tostring(g)
        group_item = group_item.. "@".. keycloak_domain
        local get_group_ok, groupObj = groupDao.getGroupByName(group_item)
        if not get_group_ok then
            njt.log(njt.ERR, "get group error: ".. group_item)
            return false, nil, nil, nil
        end
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
