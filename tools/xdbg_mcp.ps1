param([Parameter(ValueFromPipeline=$true)][int]$Id)
# 封装 x64dbg MCP 的 JSON-RPC 调用
function Invoke-Xdbg {
    param([string]$Tool, $Params = @{})
    $body = @{
        jsonrpc = '2.0'
        id      = [guid]::NewGuid().ToString().Substring(0,8)
        method  = 'tools/call'
        params  = @{ name = $Tool; arguments = $Params }
    } | ConvertTo-Json -Depth 10 -Compress

    $r = Invoke-WebRequest -Uri 'http://127.0.0.1:8888/mcp' -Method POST -Body $body `
         -ContentType 'application/json' -TimeoutSec 20 -UseBasicParsing
    $j = $r.Content | ConvertFrom-Json
    if ($j.error) { return "ERROR: $($j.error | ConvertTo-Json -Compress)" }
    # MCP 把结果包在 content[0].text 里
    if ($j.result.content) { return ($j.result.content[0].text) }
    return ($j.result | ConvertTo-Json -Depth 10 -Compress)
}