local lor = require("lor.index")
local cjson = require("cjson")
local sysConfigDao = require("api_gateway.dao.sys_config")

local realmRouter = lor:Router()


local RETURN_CODE = {
    SUCCESS = 0,
    REALM_QUERY_FAIL = 10
}

local function getRealm(req, res, next)
    local retObj = {}

    -- get realm
    local config_key = "keycloak_realm"
    local ok, msg = sysConfigDao.getSysConfigByKey(config_key)
    if not ok then
        retObj.code = RETURN_CODE.REALM_QUERY_FAIL
        retObj.msg = "keycloak_realm is not exist"
    else
        local keycloakRealmObj = {}
        keycloakRealmObj.realms = {}
        for i, g in ipairs(msg) do
            local config_value = tostring(g.config_value)
            
            for word in string.gmatch(config_value, "%S+") do
                table.insert(keycloakRealmObj.realms, word)
            end
        end

        retObj.code = RETURN_CODE.SUCCESS
        retObj.msg = "success"
        retObj.data = keycloakRealmObj
    end

    res:json(retObj, true)
end


realmRouter:get("/realm", getRealm)


return realmRouter
