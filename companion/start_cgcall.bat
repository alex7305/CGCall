@echo off
cd /d "%~dp0"
start "" /min "cgcall_feed.exe"
start "" /wait "JS8Call.exe"
taskkill /im cgcall_feed.exe /f >nul 2>&1
