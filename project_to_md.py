import os
from datetime import datetime

# --- 配置区 ---

# 要扫描的根目录 (脚本将扫描这些文件夹中的所有内容)
PROJECT_ROOTS = ['data-reader', 'data-processor']

# 输出的Markdown文件名
OUTPUT_FILENAME = 'project_source_code.md'

# 要包含的文件扩展名 (可以根据需要添加)
INCLUDE_EXTENSIONS = {
    # C/C++
    '.c', '.h',
    # Rust
    '.rs',
    # Build systems & Config
    'makefile', 'cargo.toml', 'cargo.lock',
    # Scripts
    '.ps1',
    # Documentation & Text
    '.md', '.txt'
}

# 要排除的目录和文件 (避免包含不必要的内容)
EXCLUDE_DIRS = {'__pycache__', '.git', 'target', 'debug', 'release'}
EXCLUDE_FILES = {'.ds_store', '.gitignore', 'rustup-init.exe'}

# --- 脚本实现 ---

def get_language_from_extension(filename):
    """根据文件名后缀猜测Markdown代码块的语言标识符"""
    name, ext = os.path.splitext(filename.lower())
    if ext in ['.c', '.h']:
        return 'c'
    if ext == '.rs':
        return 'rust'
    if 'makefile' in name:
        return 'makefile'
    if 'cargo.toml' in name:
        return 'toml'
    if ext == '.ps1':
        return 'powershell'
    if ext == '.md':
        return 'markdown'
    return '' # 默认为纯文本

def process_project(root_dir, md_file):
    """递归处理单个项目目录并写入到Markdown文件"""
    print(f"正在处理项目: {root_dir}...")
    
    # 对文件和目录进行排序，保证输出顺序一致
    sorted_entries = sorted(os.walk(root_dir), key=lambda x: x[0])

    for dirpath, dirnames, filenames in sorted_entries:
        # 过滤掉需要排除的目录
        dirnames[:] = [d for d in dirnames if d.lower() not in EXCLUDE_DIRS]
        
        # 对文件名排序
        filenames.sort()

        for filename in filenames:
            # 过滤掉需要排除的文件
            if filename.lower() in EXCLUDE_FILES:
                continue

            # 检查文件扩展名是否在白名单中
            file_ext = os.path.splitext(filename.lower())[1]
            if file_ext not in INCLUDE_EXTENSIONS and not any(inc_ext in filename.lower() for inc_ext in INCLUDE_EXTENSIONS):
                 continue

            file_path = os.path.join(dirpath, filename)
            relative_path = os.path.normpath(file_path).replace('\\', '/')
            
            print(f"  -> 正在添加: {relative_path}")
            
            md_file.write(f"## 文件: `{relative_path}`\n\n")
            
            try:
                with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
                    content = f.read()
                
                lang = get_language_from_extension(filename)
                
                md_file.write(f"```{lang}\n")
                md_file.write(content.strip())
                md_file.write("\n```\n\n")
                
            except Exception as e:
                md_file.write(f"```\n无法读取文件: {e}\n```\n\n")

def main():
    """主函数"""
    try:
        with open(OUTPUT_FILENAME, 'w', encoding='utf-8') as md_file:
            md_file.write(f"# 项目源代码文档\n\n")
            md_file.write(f"> 本文档由Python脚本自动生成于: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
            md_file.write("---\n\n")
            
            for project_dir in PROJECT_ROOTS:
                if os.path.isdir(project_dir):
                    process_project(project_dir, md_file)
                else:
                    print(f"警告: 目录 '{project_dir}' 不存在，已跳过。")
            
        print(f"\n✅ 成功！所有项目代码已整合到 `{OUTPUT_FILENAME}` 文件中。")
        print("您现在可以将这个Markdown文件发送给我了。")

    except IOError as e:
        print(f"\n❌ 错误: 无法写入文件 '{OUTPUT_FILENAME}'。请检查权限。")
    except Exception as e:
        print(f"\n❌ 发生未知错误: {e}")

if __name__ == "__main__":
    main()
