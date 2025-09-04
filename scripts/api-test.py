#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
数据采集系统API接口测试脚本
测试所有REST API接口和WebSocket连接
"""

import asyncio
import json
import time
import base64
import requests
import websockets
from typing import Dict, Any, Optional, List
from datetime import datetime


class APITester:
    def __init__(self, base_url: str = "http://127.0.0.1:8080", ws_url: str = "ws://127.0.0.1:8081"):
        self.base_url = base_url
        self.ws_url = ws_url
        self.session = requests.Session()
        self.test_results = []

    def log_result(self, test_name: str, success: bool, message: str = "", data: Any = None):
        """记录测试结果"""
        result = {
            "test": test_name,
            "success": success,
            "message": message,
            "timestamp": datetime.now().isoformat(),
            "data": data
        }
        self.test_results.append(result)
        
        status = "✓ PASS" if success else "✗ FAIL"
        print(f"[{status}] {test_name}: {message}")
        if data and not success:
            print(f"    Response: {data}")

    def make_request(self, method: str, endpoint: str, json_data: Dict = None, 
                    params: Dict = None) -> Optional[Dict]:
        """发送HTTP请求"""
        url = f"{self.base_url}{endpoint}"
        try:
            if method.upper() == 'GET':
                response = self.session.get(url, params=params)
            elif method.upper() == 'POST':
                response = self.session.post(url, json=json_data, params=params)
            elif method.upper() == 'DELETE':
                response = self.session.delete(url, params=params)
            else:
                raise ValueError(f"Unsupported method: {method}")
            
            response.raise_for_status()
            return response.json() if response.content else {"status": "success"}
            
        except requests.exceptions.RequestException as e:
            return {"error": str(e), "status_code": getattr(e.response, 'status_code', None)}
        except json.JSONDecodeError as e:
            return {"error": f"JSON decode error: {e}", "raw_content": response.text[:500]}

    # ========== 系统控制API测试 ==========
    
    def test_system_status(self):
        """测试系统状态接口"""
        result = self.make_request('GET', '/api/control/status')
        if result and result.get('success'):
            self.log_result("系统状态查询", True, "成功获取系统状态", result.get('data'))
        else:
            self.log_result("系统状态查询", False, "获取状态失败", result)
        return result

    def test_start_collection(self):
        """测试启动数据采集"""
        result = self.make_request('POST', '/api/control/start')
        if result and result.get('success'):
            self.log_result("启动数据采集", True, result.get('data', ''))
        else:
            self.log_result("启动数据采集", False, "启动失败", result)
        return result

    def test_stop_collection(self):
        """测试停止数据采集"""
        result = self.make_request('POST', '/api/control/stop')
        if result and result.get('success'):
            self.log_result("停止数据采集", True, result.get('data', ''))
        else:
            self.log_result("停止数据采集", False, "停止失败", result)
        return result

    def test_device_ping(self):
        """测试设备ping"""
        result = self.make_request('POST', '/api/control/ping')
        if result and result.get('success'):
            self.log_result("设备Ping测试", True, result.get('data', ''))
        else:
            self.log_result("设备Ping测试", False, "Ping失败", result)
        return result

    def test_device_info(self):
        """测试获取设备信息"""
        result = self.make_request('POST', '/api/control/device_info')
        if result and result.get('success'):
            self.log_result("获取设备信息", True, result.get('data', ''))
        else:
            self.log_result("获取设备信息", False, "获取失败", result)
        return result

    # ========== 模式控制API测试 ==========
    
    def test_continuous_mode(self):
        """测试连续模式设置"""
        result = self.make_request('POST', '/api/control/continuous_mode')
        if result and result.get('success'):
            self.log_result("设置连续模式", True, result.get('data', ''))
        else:
            self.log_result("设置连续模式", False, "设置失败", result)
        return result

    def test_trigger_mode(self):
        """测试触发模式设置"""
        result = self.make_request('POST', '/api/control/trigger_mode')
        if result and result.get('success'):
            self.log_result("设置触发模式", True, result.get('data', ''))
        else:
            self.log_result("设置触发模式", False, "设置失败", result)
        return result

    def test_request_trigger_data(self):
        """测试请求触发数据"""
        # 首先设置触发模式
        self.test_trigger_mode()
        time.sleep(1)
        
        result = self.make_request('POST', '/api/control/request_trigger_data')
        if result and result.get('success'):
            self.log_result("请求触发数据", True, result.get('data', ''))
        else:
            self.log_result("请求触发数据", False, "请求失败", result)
        return result

    def test_configure_stream(self):
        """测试配置数据流"""
        config_data = {
            "channels": [
                {
                    "channel_id": 0,
                    "sample_rate": 10000,
                    "format": 1  # int16
                },
                {
                    "channel_id": 1,
                    "sample_rate": 10000,
                    "format": 1  # int16
                }
            ]
        }
        
        result = self.make_request('POST', '/api/control/configure', config_data)
        if result and result.get('success'):
            self.log_result("配置数据流", True, result.get('data', ''))
        else:
            self.log_result("配置数据流", False, "配置失败", result)
        return result

    # ========== 触发数据管理API测试 ==========
    
    def test_trigger_list(self):
        """测试获取触发批次列表"""
        result = self.make_request('GET', '/api/trigger/list')
        if result and result.get('success'):
            bursts = result.get('data', [])
            self.log_result("获取触发批次列表", True, f"找到{len(bursts)}个批次")
            return bursts
        else:
            self.log_result("获取触发批次列表", False, "获取失败", result)
            return []

    def test_trigger_preview(self, burst_id: str):
        """测试预览触发批次"""
        if not burst_id:
            self.log_result("预览触发批次", False, "没有可预览的批次ID")
            return None
            
        result = self.make_request('GET', f'/api/trigger/preview/{burst_id}')
        if result and result.get('success'):
            self.log_result("预览触发批次", True, f"成功预览批次 {burst_id}")
            return result.get('data')
        else:
            self.log_result("预览触发批次", False, f"预览失败: {burst_id}", result)
            return None

    def test_trigger_save(self, burst_id: str):
        """测试保存触发批次"""
        if not burst_id:
            self.log_result("保存触发批次", False, "没有可保存的批次ID")
            return None
            
        save_data = {
            "dir": "test_output",
            "filename": f"test_burst_{int(time.time())}",
            "format": "json",
            "description": "Python测试脚本生成的测试数据"
        }
        
        result = self.make_request('POST', f'/api/trigger/save/{burst_id}', save_data)
        if result and result.get('success'):
            saved_path = result.get('data', {}).get('saved_path', '')
            self.log_result("保存触发批次", True, f"已保存到: {saved_path}")
            return result.get('data')
        else:
            self.log_result("保存触发批次", False, f"保存失败: {burst_id}", result)
            return None

    def test_trigger_delete(self, burst_id: str):
        """测试删除触发批次"""
        if not burst_id:
            self.log_result("删除触发批次", False, "没有可删除的批次ID")
            return None
            
        result = self.make_request('DELETE', f'/api/trigger/delete/{burst_id}')
        if result and result.get('success'):
            self.log_result("删除触发批次", True, f"已删除批次: {burst_id}")
        else:
            self.log_result("删除触发批次", False, f"删除失败: {burst_id}", result)
        return result

    # ========== 文件管理API测试 ==========
    
    def test_list_files(self):
        """测试列出文件"""
        # 测试根目录
        result = self.make_request('GET', '/api/files')
        if result and result.get('success'):
            files = result.get('data', [])
            self.log_result("列出根目录文件", True, f"找到{len(files)}个文件")
        else:
            self.log_result("列出根目录文件", False, "列出失败", result)
        
        # 测试子目录（如果存在）
        result_sub = self.make_request('GET', '/api/files', params={'dir': 'test_output'})
        if result_sub and result_sub.get('success'):
            files_sub = result_sub.get('data', [])
            self.log_result("列出子目录文件", True, f"找到{len(files_sub)}个文件")
        else:
            self.log_result("列出子目录文件", False, "子目录不存在或为空")
        
        return result

    def test_save_file(self):
        """测试保存数据文件"""
        # 创建测试数据
        test_data = "这是一个Python测试文件\n生成时间: " + datetime.now().isoformat()
        base64_data = base64.b64encode(test_data.encode('utf-8')).decode('ascii')
        
        save_data = {
            "dir": "test_output",
            "filename": f"python_test_{int(time.time())}.txt",
            "base64": base64_data
        }
        
        result = self.make_request('POST', '/api/files/save', save_data)
        if result and result.get('success'):
            saved_path = result.get('data', '')
            self.log_result("保存数据文件", True, f"已保存到: {saved_path}")
            return saved_path
        else:
            self.log_result("保存数据文件", False, "保存失败", result)
            return None

    def test_download_file(self, filename: str):
        """测试下载文件"""
        if not filename:
            self.log_result("下载文件", False, "没有可下载的文件名")
            return None
            
        try:
            # 对文件名进行URL编码，处理包含斜杠的路径
            import urllib.parse
            encoded_filename = urllib.parse.quote(filename, safe='')
            url = f"{self.base_url}/api/files/{encoded_filename}"
            response = self.session.get(url)
            
            if response.status_code == 200:
                content_length = len(response.content)
                self.log_result("下载文件", True, f"成功下载 {filename} ({content_length} bytes)")
                return response.content
            else:
                self.log_result("下载文件", False, f"下载失败 {filename}: HTTP {response.status_code}")
                # 尝试不编码的方式（备用方案）
                if '/' in filename:
                    try:
                        url_alt = f"{self.base_url}/api/files/{filename}"
                        response_alt = self.session.get(url_alt)
                        if response_alt.status_code == 200:
                            content_length = len(response_alt.content)
                            self.log_result("下载文件-备用方案", True, f"成功下载 {filename} ({content_length} bytes)")
                            return response_alt.content
                    except:
                        pass
                return None
                
        except Exception as e:
            self.log_result("下载文件", False, f"下载异常: {e}")
            return None

    # ========== 系统信息API测试 ==========
    
    def test_health_check(self):
        """测试健康检查"""
        result = self.make_request('GET', '/health')
        if result and result.get('success'):
            health_data = result.get('data', {})
            status = health_data.get('status', 'unknown')
            version = health_data.get('version', 'unknown')
            self.log_result("健康检查", True, f"系统状态: {status}, 版本: {version}")
        else:
            self.log_result("健康检查", False, "健康检查失败", result)
        return result

    def test_api_info(self):
        """测试API信息"""
        result = self.make_request('GET', '/')
        if result and 'name' in result:
            name = result.get('name', 'unknown')
            version = result.get('version', 'unknown')
            self.log_result("API信息查询", True, f"系统: {name} v{version}")
        else:
            self.log_result("API信息查询", False, "获取信息失败", result)
        return result

    # ========== WebSocket测试 ==========
    
    async def test_websocket_connection(self):
        """测试WebSocket连接和消息"""
        try:
            async with websockets.connect(self.ws_url) as websocket:
                self.log_result("WebSocket连接", True, "连接建立成功")
                
                # 等待欢迎消息
                welcome_msg = await asyncio.wait_for(websocket.recv(), timeout=5.0)
                welcome_data = json.loads(welcome_msg)
                
                if welcome_data.get('type') == 'welcome':
                    client_id = welcome_data.get('client_id')
                    self.log_result("WebSocket欢迎消息", True, f"客户端ID: {client_id}")
                else:
                    self.log_result("WebSocket欢迎消息", False, "未收到预期的欢迎消息")
                
                # 发送订阅消息
                subscribe_msg = {
                    "type": "subscribe",
                    "channels": ["all"]
                }
                await websocket.send(json.dumps(subscribe_msg))
                self.log_result("WebSocket订阅", True, "已发送订阅请求")
                
                # 发送ping消息
                ping_msg = {"type": "ping"}
                await websocket.send(json.dumps(ping_msg))
                
                # 等待响应消息
                message_count = 0
                start_time = time.time()
                
                while message_count < 5 and (time.time() - start_time) < 10:
                    try:
                        message = await asyncio.wait_for(websocket.recv(), timeout=2.0)
                        msg_data = json.loads(message)
                        msg_type = msg_data.get('type', 'unknown')
                        
                        self.log_result(f"WebSocket消息-{msg_type}", True, 
                                      f"收到消息类型: {msg_type}")
                        message_count += 1
                        
                    except asyncio.TimeoutError:
                        break
                
                if message_count > 0:
                    self.log_result("WebSocket消息接收", True, f"共收到{message_count}条消息")
                else:
                    self.log_result("WebSocket消息接收", False, "超时未收到消息")
                    
        except Exception as e:
            self.log_result("WebSocket连接", False, f"连接失败: {e}")

    # ========== 综合测试流程 ==========
    
    def run_all_tests(self):
        """运行所有测试"""
        print("=" * 60)
        print("开始API接口测试")
        print("=" * 60)
        
        # 1. 基础系统测试
        print("\n[1] 基础系统测试")
        print("-" * 30)
        self.test_health_check()
        self.test_api_info()
        self.test_system_status()
        
        # 2. 设备控制测试
        print("\n[2] 设备控制测试")
        print("-" * 30)
        self.test_device_ping()
        self.test_device_info()
        
        # 3. 模式控制测试
        print("\n[3] 模式控制测试")
        print("-" * 30)
        self.test_configure_stream()
        self.test_continuous_mode()
        time.sleep(1)  # 等待模式切换
        self.test_trigger_mode()
        time.sleep(1)  # 等待模式切换
        
        # 4. 数据采集控制测试
        print("\n[4] 数据采集控制测试")
        print("-" * 30)
        self.test_start_collection()
        time.sleep(2)  # 让系统运行一会儿
        self.test_request_trigger_data()
        time.sleep(1)
        self.test_stop_collection()
        
        # 5. 触发数据管理测试
        print("\n[5] 触发数据管理测试")
        print("-" * 30)
        trigger_bursts = self.test_trigger_list()
        if trigger_bursts:
            # 测试第一个批次
            first_burst = trigger_bursts[0]
            burst_id = first_burst.get('burst_id')
            if burst_id:
                self.test_trigger_preview(burst_id)
                self.test_trigger_save(burst_id)
                # 注意：这里不删除，避免破坏数据
                # self.test_trigger_delete(burst_id)
        
        # 6. 文件管理测试
        print("\n[6] 文件管理测试")
        print("-" * 30)
        saved_file = self.test_save_file()
        self.test_list_files()
        if saved_file:
            self.test_download_file(saved_file)
        
        # 7. WebSocket测试
        print("\n[7] WebSocket测试")
        print("-" * 30)
        try:
            asyncio.run(self.test_websocket_connection())
        except Exception as e:
            self.log_result("WebSocket测试", False, f"测试异常: {e}")

    def print_summary(self):
        """打印测试结果摘要"""
        print("\n" + "=" * 60)
        print("测试结果摘要")
        print("=" * 60)
        
        total_tests = len(self.test_results)
        passed_tests = sum(1 for r in self.test_results if r['success'])
        failed_tests = total_tests - passed_tests
        
        print(f"总测试数: {total_tests}")
        print(f"通过: {passed_tests}")
        print(f"失败: {failed_tests}")
        print(f"成功率: {(passed_tests/total_tests)*100:.1f}%")
        
        if failed_tests > 0:
            print(f"\n失败的测试:")
            for result in self.test_results:
                if not result['success']:
                    print(f"  - {result['test']}: {result['message']}")
        
        print("\n测试完成!")

    def save_results(self, filename: str = "test_results.json"):
        """保存测试结果到文件"""
        try:
            with open(filename, 'w', encoding='utf-8') as f:
                json.dump({
                    'summary': {
                        'total_tests': len(self.test_results),
                        'passed_tests': sum(1 for r in self.test_results if r['success']),
                        'failed_tests': sum(1 for r in self.test_results if not r['success']),
                        'test_time': datetime.now().isoformat()
                    },
                    'results': self.test_results
                }, f, indent=2, ensure_ascii=False)
            print(f"\n测试结果已保存到: {filename}")
        except Exception as e:
            print(f"\n保存测试结果失败: {e}")


def main():
    """主函数"""
    # 可以通过命令行参数修改地址
    import sys
    
    base_url = "http://127.0.0.1:8080"
    ws_url = "ws://127.0.0.1:8081"
    
    if len(sys.argv) > 1:
        base_url = sys.argv[1]
    if len(sys.argv) > 2:
        ws_url = sys.argv[2]
    
    print(f"API服务地址: {base_url}")
    print(f"WebSocket地址: {ws_url}")
    
    # 创建测试器并运行测试
    tester = APITester(base_url, ws_url)
    
    try:
        tester.run_all_tests()
        tester.print_summary()
        tester.save_results()
        
    except KeyboardInterrupt:
        print("\n\n测试被用户中断")
        tester.print_summary()
    except Exception as e:
        print(f"\n\n测试过程发生异常: {e}")
        tester.print_summary()


if __name__ == "__main__":
    main()