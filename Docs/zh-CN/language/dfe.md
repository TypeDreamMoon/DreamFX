---
title: .dfe — 可复用 emitter
description: 一个 Emitter 文件、from 的合并粒度，以及「拷贝而不是继承」这个决定的两个后果。
---

一个 `.dfe` 装一个 `Emitter` 块，写法和它内联在 `.dfs` 里时一模一样：

```cpp
Emitter(Name="Emitters/E_MoonFlashCard", Root="Plugin.DreamFX")
{
    Settings = { SimTarget = CPU; LocalSpace = true; QualityLevelMask = 31; }

    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.12);
        SpawnBurst_Instantaneous(SpawnCount = 1);
    }

    ParticleSpawn  = { ... }
    ParticleUpdate = { ... }

    SpriteRenderer Card { ... }
}
```

`QualityLevelMask` 是可扩展性位掩码，它住在 emitter 的 `FNiagaraPlatformSet` 里而不是顶层。
属性藏在结构体里的设置，内部用点号路径寻址，而 DSL 名取**叶子** —— 因为那才是作者要设的那一段。

<a id="它自己不产资产"></a>

## 它自己不产资产

一个 `.dfe` 单独构建什么都不产（[DFX5097](../diagnostics/DFX5xxx.md#dfx5097)）。
它的存在是为了被拉进一个系统：

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard"
{
    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.08);
    }
}
```

路径先相对引用它的文件解析，再相对每一个 DFX 根解析。扩展名可省。

<a id="拷贝不是继承"></a>

## 拷贝，不是继承

`from` 拿的是**快照**。之后再改 `.dfe`，已经拷过它的系统不会有任何变化，直到那些系统重建；
生成出来的资产也不带回指的链接。

这个决定还有第二个后果：**反编译出来的系统永远是自包含的**。拷贝一旦发生，
「这些里面哪些是覆盖」就不可恢复了，所以导出重建不出 `from`。它是单向的语法糖。

<a id="覆盖块替换什么"></a>

## 覆盖块替换什么

合并粒度是**块**，不是模块：

| 部分 | 合并方式 |
| --- | --- |
| `Settings` | **逐键** —— 名字没有歧义，覆盖只替换那一个设置 |
| 每个栈 | 宿主声明了就**整块替换**；没声明就保留 `.dfe` 的 |
| renderer | **全有或全无** —— 声明任何一个 renderer 就替换掉全部 |

栈之所以整块替换，是因为按模块合并需要一个这门语言没有的模块身份：同一个栈里对同一个模块的两次调用
无法区分，合并就只能猜，而猜错会静默地把别人的特效重新排序。

renderer 的全有全无是同一个理由的另一面：它们按声明顺序寻址，替换三个里的一个会静默地把另外两个
重新编号。

<a id="user-参数"></a>

## user 参数

一个 `.dfe` 可以读 `User.*`。宿主系统必须声明这些参数，而检查发生在 **`from` 那一行**，
不在 `.dfe` 内部 —— 因为「把它拉进来」这个决定是在那里做的
（[DFX3043](../diagnostics/DFX3xxx.md#dfx3043)）。

```text
NS_Host.dfs(7,5): error DFX3043: '../Emitters/E_Flash' reads user parameters this system does not
declare: User.FlashCount. Add them to the Properties block.
```

<a id="从已有-emitter-导出一个-dfe"></a>

## 从已有 emitter 导出一个 `.dfe`

Content Browser 里右键一个 `UNiagaraEmitter` → **DreamFX ▸ Export .dfe**。

它把 emitter 拷进一个一次性的 `/Temp` 宿主系统，在那里读，再写出文件。这一步是它诚实而不是猜测的
原因：Niagara 外部编辑 API 的每一个读取器都通过一个拥有它的系统寻址，所以读回来的正是
「某个系统用它时，它贡献了什么」。见[编辑器集成](../tools/editor-integration.md)。
