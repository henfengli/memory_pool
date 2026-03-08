# Git 版本控制讲解

> 本文档面向 Git 初学者，结合 mempool 项目的实际操作，讲解 Git 版本控制的核心概念和常用命令。

---

## 一、Git 是什么？

Git 是一个**分布式版本控制系统**，用来追踪文件的每一次修改。它的核心价值：

1. **历史回溯**：任何时候都能回到过去的某个版本
2. **多人协作**：多人同时修改同一个项目不会冲突
3. **分支管理**：可以同时开发多个功能而互不干扰

### 核心概念图

```
工作目录 (Working Directory)
    │
    │ git add
    ▼
暂存区 (Staging Area / Index)
    │
    │ git commit
    ▼
本地仓库 (Local Repository)
    │
    │ git push
    ▼
远程仓库 (Remote Repository, 如 GitHub)
```

你的代码修改经过三个阶段才到达远程仓库：
- **工作目录**：你正在编辑的文件
- **暂存区**：用 `git add` 标记"这些改动我要提交"
- **本地仓库**：用 `git commit` 把暂存的改动保存为一个版本快照
- **远程仓库**：用 `git push` 把本地版本推送到 GitHub

---

## 二、本项目的 Git 操作实录

### 2.1 初始化仓库

```bash
cd F:/projects/memorypool

# 初始化一个空的 Git 仓库（在当前目录下创建 .git/ 隐藏文件夹）
git init

# 添加远程仓库地址，起名为 origin
git remote add origin git@github.com:henfengli/memory_pool.git
```

- `git init`：把当前目录变成 Git 仓库。执行后会多出一个 `.git/` 目录，里面存放所有版本信息。
- `git remote add`：告诉 Git"我的代码要推送到哪个远程服务器"。`origin` 是远程仓库的别名（约定俗成的默认名）。

### 2.2 创建 .gitignore

`.gitignore` 文件告诉 Git 哪些文件**不需要**被版本控制：

```gitignore
# 构建产物（每次编译都会重新生成，不需要存到仓库）
build/

# IDE 配置（每个人的 IDE 设置不同）
.vscode/
.idea/

# 编译中间文件
*.o
*.obj
*.exe
*.dll
```

**为什么需要 .gitignore？**
- `.exe`、`.dll` 等二进制文件体积大、每次编译都变，存入 Git 会导致仓库膨胀
- IDE 配置是个人环境，不应强制同步给所有人
- 构建目录 `build/` 随时可以重新生成

### 2.3 提交 v1.0.0（初始版本）

```bash
# 第一步：把文件添加到暂存区
git add .gitignore CMakeLists.txt src/ include/ test/ examples/ docs/

# 第二步：查看暂存状态（确认要提交哪些文件）
git status

# 第三步：提交到本地仓库
git commit -m "feat: mempool v1.0.0 - 多维度小型内存池初始实现"

# 第四步：打标签
git tag -a v1.0.0 -m "mempool v1.0.0 - 初始版本"
```

逐条解释：

| 命令 | 作用 |
|------|------|
| `git add <文件>` | 把指定文件/目录加入暂存区。可以用 `git add .` 添加所有文件，但建议指定具体文件以避免误添加敏感文件 |
| `git status` | 显示当前状态：哪些文件被修改、哪些已暂存、哪些未跟踪 |
| `git commit -m "消息"` | 把暂存区的内容保存为一个"快照"（commit）。`-m` 后面是描述这次改动的消息 |
| `git tag -a v1.0.0 -m "..."` | 给当前 commit 打一个**带注解的标签**。标签像是一个"书签"，标记重要的版本节点 |

### 2.4 提交 v1.1.0（性能优化版本）

```bash
# 用优化后的代码替换原有文件（修改 src/arena.cpp、src/tlc.cpp 等）

# 查看有哪些文件被修改
git diff --stat

# 暂存所有变更
git add -A src/ include/ test/ examples/ docs/ CMakeLists.txt

# 提交
git commit -m "perf: mempool v1.1.0 - 性能优化"

# 打标签
git tag -a v1.1.0 -m "mempool v1.1.0 - 性能优化版本"
```

### 2.5 推送到 GitHub

```bash
# 推送 master 分支到远程
git push -u origin master

# 推送所有标签
git push origin --tags
```

- `git push -u origin master`：把本地 `master` 分支推送到远程 `origin`。`-u` 表示"设为默认上游"，以后直接 `git push` 就行了。
- `git push origin --tags`：标签不会随 `git push` 自动推送，需要单独推送。

---

## 三、如何查看历史版本

### 3.1 查看提交日志

```bash
# 精简格式（一行一个 commit）
git log --oneline --decorate

# 输出示例：
# d531558 (HEAD -> master, tag: v1.1.0) perf: mempool v1.1.0 - 性能优化
# 9825abe (tag: v1.0.0) feat: mempool v1.0.0 - 多维度小型内存池初始实现
```

- `d531558` 是 commit 的唯一 ID（哈希值的前 7 位）
- `HEAD -> master` 表示当前所在位置
- `tag: v1.1.0` 表示这个 commit 被标记为 v1.1.0

### 3.2 查看两个版本之间的差异

```bash
# 查看 v1.0.0 和 v1.1.0 之间修改了哪些文件
git diff v1.0.0 v1.1.0 --stat

# 查看某个文件的具体改动
git diff v1.0.0 v1.1.0 -- src/arena.cpp
```

### 3.3 切换到旧版本查看代码

```bash
# 切换到 v1.0.0 查看初始版本代码
git checkout v1.0.0

# 此时处于"分离 HEAD"状态，可以查看代码但不要修改
# 切回最新版本：
git checkout master
```

### 3.4 列出所有标签

```bash
git tag -l -n1

# 输出：
# v1.0.0  mempool v1.0.0 - 初始版本
# v1.1.0  mempool v1.1.0 - 性能优化版本
```

---

## 四、Git 核心概念详解

### 4.1 Commit（提交）

一个 commit 就是项目在某个时刻的**完整快照**。每个 commit 包含：
- 一个唯一的 SHA-1 哈希 ID（如 `9825abe`）
- 作者和时间
- 提交消息（描述做了什么）
- 指向父 commit 的指针（形成链表）

```
commit 9825abe          commit d531558
(v1.0.0)    ───────►    (v1.1.0)
初始实现                 性能优化
```

### 4.2 Tag（标签）

标签是指向某个 commit 的**永久指针**，用于标记发布版本。

- **轻量标签**：`git tag v1.0.0` — 只是一个指针
- **带注解标签**：`git tag -a v1.0.0 -m "..."` — 包含作者、日期、描述信息（推荐使用）

### 4.3 Branch（分支）

分支是一条独立的开发线。本项目只用了 `master` 分支，但实际开发中常见的分支策略：

```
master:    ──●──────●──────●──────●
              \                  /
feature:       ──●──●──●──●──●──
                  (开发新功能)
```

常用分支命令：
```bash
git branch feature-xxx      # 创建分支
git checkout feature-xxx     # 切换到分支
git checkout -b feature-xxx  # 创建并切换（上面两步合一）
git merge feature-xxx        # 把分支合并回当前分支
git branch -d feature-xxx    # 删除已合并的分支
```

### 4.4 .git 目录结构

```
.git/
├── HEAD            # 指向当前所在的分支
├── config          # 仓库配置（远程地址等）
├── objects/        # 所有版本的文件内容（压缩存储）
├── refs/
│   ├── heads/      # 分支指针
│   └── tags/       # 标签指针
├── index           # 暂存区
└── logs/           # 操作日志（用于 reflog）
```

---

## 五、常用 Git 命令速查表

### 基础操作

| 命令 | 作用 | 示例 |
|------|------|------|
| `git init` | 初始化仓库 | `git init` |
| `git clone <url>` | 克隆远程仓库 | `git clone git@github.com:user/repo.git` |
| `git status` | 查看当前状态 | `git status` |
| `git add <file>` | 添加到暂存区 | `git add src/arena.cpp` |
| `git commit -m "msg"` | 提交 | `git commit -m "fix: 修复内存泄漏"` |
| `git push` | 推送到远程 | `git push origin master` |
| `git pull` | 拉取远程更新 | `git pull origin master` |

### 查看信息

| 命令 | 作用 |
|------|------|
| `git log --oneline` | 查看提交历史（精简） |
| `git log --graph --oneline` | 以图形化方式查看分支历史 |
| `git diff` | 查看未暂存的修改 |
| `git diff --cached` | 查看已暂存但未提交的修改 |
| `git diff v1.0.0 v1.1.0` | 查看两个版本的差异 |
| `git show v1.0.0` | 查看某个 tag/commit 的详细信息 |

### 版本回退

| 命令 | 作用 | 危险性 |
|------|------|--------|
| `git checkout <tag>` | 查看旧版本（只读） | 安全 |
| `git revert <commit>` | 创建一个"撤销"某次提交的新 commit | 安全 |
| `git reset --soft HEAD~1` | 撤销最近一次 commit（保留修改） | 中等 |
| `git reset --hard HEAD~1` | 撤销最近一次 commit（**丢弃修改**） | **危险** |

### 标签操作

| 命令 | 作用 |
|------|------|
| `git tag -a v1.0.0 -m "msg"` | 创建带注解标签 |
| `git tag -l` | 列出所有标签 |
| `git push origin --tags` | 推送所有标签到远程 |
| `git checkout v1.0.0` | 切换到某个标签 |

---

## 六、Commit 消息规范

本项目采用 **Conventional Commits** 规范：

```
<类型>: <简短描述>

<详细描述（可选）>
```

常用类型前缀：

| 前缀 | 含义 | 示例 |
|------|------|------|
| `feat` | 新功能 | `feat: 实现 Arena 分区管理` |
| `fix` | 修复 bug | `fix: 修复跨线程 free 双重释放` |
| `perf` | 性能优化 | `perf: bitmap 扫描引入 hint 加速` |
| `refactor` | 重构（不改变功能） | `refactor: 拆分 TLC 模块` |
| `test` | 添加测试 | `test: 增加多线程压力测试` |
| `docs` | 文档 | `docs: 补充 API 使用说明` |
| `chore` | 杂务（构建、配置等） | `chore: 更新 CMakeLists.txt` |

---

## 七、本项目的版本管理策略

```
时间线 ──────────────────────────────────────────►

master:  ●────────────────────●
         │                    │
     v1.0.0               v1.1.0
     初始实现              性能优化
     (9825abe)            (d531558)
```

- **v1.0.0**：完整的初始版本，包含所有核心功能
- **v1.1.0**：在 v1.0.0 基础上优化性能，核心改动集中在 `arena.cpp`、`tlc.cpp`

通过 `git checkout v1.0.0` 可以随时回到优化前的代码查看对比，通过 `git diff v1.0.0 v1.1.0` 可以精确看到每一行改动。

---

## 八、在 GitHub 上查看

推送完成后，你可以在 GitHub 上：

1. **查看代码**：`https://github.com/henfengli/memory_pool`
2. **查看版本**：点击 "Releases" 或 "Tags" 标签页
3. **对比两个版本**：`https://github.com/henfengli/memory_pool/compare/v1.0.0...v1.1.0`
4. **下载某个版本的源码**：在 Tags 页面点击对应版本的 "Source code (zip)"
