#!/usr/bin/env python3
"""Generate muse_pinyin.c/.h and a host-side unit test."""
from __future__ import annotations

import os
import textwrap

OUT_DIR = os.environ.get("OUT_DIR", "/workspace/github-upload/muse-gadget-185b-push/esp32/components/muse")
TEST_DIR = os.environ.get("TEST_DIR", "/workspace/ime-rewrite/tests")

# Expanded everyday + Muse/gadget + stock/chat vocabulary (toneless pinyin -> hanzi).
# Keep reasonably sized: ~700 words.
WORDS: list[tuple[str, str]] = [
    ("agu", "A股"), ("aihao", "爱好"), ("anquan", "安全"), ("anpai", "安排"),
    ("baifenbi", "百分比"), ("bangong", "办公"), ("banben", "版本"), ("bangding", "绑定"),
    ("bangzhu", "帮助"), ("baocun", "保存"), ("baojing", "报警"), ("baoqian", "抱歉"),
    ("baoyang", "保养"), ("beijing", "北京"), ("benneng", "本能"), ("biaoti", "标题"),
    ("biaoxian", "表现"), ("biekeqi", "别客气"), ("bingqie", "并且"), ("bixu", "必须"),
    ("bukan", "不看"), ("budui", "不对"), ("buhaoyisi", "不好意思"), ("buneng", "不能"),
    ("buyao", "不要"), ("buyong", "不用"), ("caidan", "菜单"), ("caozuo", "操作"),
    ("ceshi", "测试"), ("chaoxi", "抄袭"), ("chaosong", "抄送"), ("chenggong", "成功"),
    ("chengjiao", "成交"), ("chengshi", "城市"), ("chifan", "吃饭"), ("chongdian", "充电"),
    ("chongqi", "重启"), ("chuxian", "出现"), ("cuowu", "错误"), ("daan", "答案"),
    ("dabiao", "达标"), ("daima", "代码"), ("dakai", "打开"), ("danshi", "但是"),
    ("dazi", "打字"), ("dengdai", "等待"), ("dengdeng", "等等"), ("denglu", "登录"),
    ("dianchi", "电池"), ("dianhua", "电话"), ("dianliang", "电量"), ("diannao", "电脑"),
    ("dianya", "电压"), ("dianying", "电影"), ("dianzan", "点赞"), ("diefu", "跌幅"),
    ("dingyue", "订阅"), ("duankai", "断开"), ("duibuqi", "对不起"), ("duihuakuang", "对话框"),
    ("duoyu", "多余"), ("erqie", "而且"), ("fanhui", "返回"), ("fanxiang", "反向"),
    ("fanyi", "翻译"), ("fayan", "发言"), ("fazhan", "发展"), ("feiji", "飞机"),
    ("fenxiang", "分享"), ("fensi", "粉丝"), ("fenzhong", "分钟"), ("fuwu", "服务"),
    ("fuwuqi", "服务器"), ("fumu", "父母"), ("gangbi", "港币"), ("ganjin", "赶紧"),
    ("ganxie", "感谢"), ("gaosu", "告诉"), ("gaoxing", "高兴"), ("gengxin", "更新"),
    ("gongsi", "公司"), ("gongzuo", "工作"), ("goumai", "购买"), ("guanbi", "关闭"),
    ("guangzhou", "广州"), ("guanzhu", "关注"), ("gupiao", "股票"), ("gupiaoda", "股票代码"),
    ("gushi", "故事"), ("guzhang", "故障"), ("haipa", "害怕"), ("haishi", "还是"),
    ("haitao", "海涛"), ("haizi", "孩子"), ("hangqing", "行情"), ("haode", "好的"),
    ("heshui", "喝水"), ("hexie", "和谐"), ("hongbao", "红包"), ("houmian", "后面"),
    ("huanxin", "欢心"), ("huanying", "欢迎"), ("huida", "回答"), ("huifu", "回复"),
    ("huisuo", "会所"), ("huoche", "火车"), ("huozhe", "或者"), ("jiage", "价格"),
    ("jiancha", "检查"), ("jiandan", "简单"), ("jianshe", "建设"), ("jiating", "家庭"),
    ("jiebang", "解绑"), ("jieshi", "解释"), ("jieshu", "结束"), ("jijin", "基金"),
    ("jilu", "记录"), ("jindu", "进度"), ("jingqing", "敬请"), ("jingya", "惊讶"),
    ("jingyin", "静音"), ("jintian", "今天"), ("jiqiren", "机器人"), ("jixu", "继续"),
    ("jizhu", "记住"), ("kafei", "咖啡"), ("kaishi", "开始"), ("kaixin", "开心"),
    ("kanjian", "看见"), ("kebushi", "可不是"), ("keneng", "可能"), ("keyi", "可以"),
    ("kongjian", "空间"), ("kuaidian", "快点"), ("kuaijie", "快捷"), ("kuisun", "亏损"),
    ("laoshi", "老师"), ("lianjie", "连接"), ("lianxi", "联系"), ("liangdu", "亮度"),
    ("liaotian", "聊天"), ("lingchen", "凌晨"), ("liulan", "浏览"), ("lixian", "离线"),
    ("maichu", "卖出"), ("mairu", "买入"), ("manman", "慢慢"), ("mashang", "马上"),
    ("meiguanxi", "没关系"), ("meigu", "美股"), ("meiguo", "美国"), ("meishi", "没事"),
    ("meitian", "每天"), ("meiyou", "没有"), ("meiyuan", "美元"), ("mima", "密码"),
    ("mingbai", "明白"), ("mingtian", "明天"), ("mubiao", "目标"), ("muse", "缪斯"),
    ("nali", "哪里"), ("name", "那么"), ("nanguo", "难过"), ("nanpengyou", "男朋友"),
    ("neirong", "内容"), ("nengfou", "能否"), ("nihao", "你好"), ("nimen", "你们"),
    ("ninhao", "您好"), ("nvpengyou", "女朋友"), ("ouyuan", "欧元"), ("ouzhou", "欧洲"),
    ("paihang", "排行"), ("peizhi", "配置"), ("pengyou", "朋友"), ("pinyin", "拼音"),
    ("pingjia", "评价"), ("pinglun", "评论"), ("pingmu", "屏幕"), ("putong", "普通"),
    ("qianbao", "钱包"), ("qianmian", "前面"), ("qichuang", "起床"), ("qidong", "启动"),
    ("qingchu", "清除"), ("qingkong", "清空"), ("qinglang", "晴朗"), ("qingqiu", "请求"),
    ("qingwen", "请问"), ("qita", "其他"), ("qizi", "妻子"), ("queren", "确认"),
    ("quxiao", "取消"), ("ranhou", "然后"), ("redian", "热点"), ("remen", "热门"),
    ("renminbi", "人民币"), ("renwu", "任务"), ("rexiao", "热销"), ("riben", "日本"),
    ("riqi", "日期"), ("riyuen", "日元"), ("rongyi", "容易"), ("ruguo", "如果"),
    ("shanchu", "删除"), ("shangban", "上班"), ("shangchuan", "上传"), ("shanghai", "上海"),
    ("shangwu", "上午"), ("shangxia", "上下"), ("shaodeng", "稍等"), ("shenme", "什么"),
    ("shenqing", "申请"), ("shengji", "升级"), ("shengqi", "生气"), ("shengyin", "声音"),
    ("shenzhen", "深圳"), ("shezhi", "设置"), ("shibai", "失败"), ("shichang", "市场"),
    ("shijian", "时间"), ("shipin", "视频"), ("shiyong", "使用"), ("shizhi", "市值"),
    ("shoucang", "收藏"), ("shouji", "手机"), ("shouye", "首页"), ("shuaxin", "刷新"),
    ("shuijiao", "睡觉"), ("shuju", "数据"), ("shuru", "输入"), ("sousuo", "搜索"),
    ("suiran", "虽然"), ("suoyi", "所以"), ("taiwan", "台湾"), ("taoyan", "讨厌"),
    ("tianjia", "添加"), ("tianqi", "天气"), ("tingzhi", "停止"), ("tongshi", "同事"),
    ("tongxun", "通讯"), ("tongzhi", "通知"), ("touzi", "投资"), ("tuichu", "退出"),
    ("tuijian", "推荐"), ("tupian", "图片"), ("wancheng", "完成"), ("wangluo", "网络"),
    ("wanjia", "玩家"), ("wanshang", "晚上"), ("weishenme", "为什么"), ("weixin", "微信"),
    ("weixiu", "维修"), ("wendu", "温度"), ("wenti", "问题"), ("wenzi", "文字"),
    ("wojia", "我家"), ("women", "我们"), ("wuxian", "无线"), ("xiabu", "下部"),
    ("xianshi", "显示"), ("xianggang", "香港"), ("xiangmu", "项目"), ("xiangqing", "详情"),
    ("xianjia", "现价"), ("xianzai", "现在"), ("xiaohua", "笑话"), ("xiaoshi", "小时"),
    ("xiaoshihou", "小时候"), ("xiaoxi", "消息"), ("xiaoyu", "下雨"), ("xiaozhu", "小助"),
    ("xiawu", "下午"), ("xiazai", "下载"), ("xiexie", "谢谢"), ("xihuan", "喜欢"),
    ("xingde", "行的"), ("xingqi", "星期"), ("xingqu", "兴趣"), ("xinwen", "新闻"),
    ("xinxin", "信心"), ("xiuxi", "休息"), ("xuanzhe", "选择"), ("xuesheng", "学生"),
    ("xuexi", "学习"), ("yanchi", "延迟"), ("yanse", "颜色"), ("yanzheng", "验证"),
    ("yaoqing", "邀请"), ("yichang", "异常"), ("yichu", "移除"), ("yiding", "一定"),
    ("yihou", "以后"), ("yijing", "已经"), ("yijian", "意见"), ("yinggai", "应该"),
    ("yingli", "盈利"), ("yingwen", "英文"), ("yingyong", "应用"), ("yinliang", "音量"),
    ("yinwei", "因为"), ("yinyue", "音乐"), ("yiqian", "以前"), ("yonghu", "用户"),
    ("youhui", "优惠"), ("youjian", "邮件"), ("youxi", "游戏"), ("youxian", "有线"),
    ("youxiang", "邮箱"), ("yuanjia", "原价"), ("yundong", "运动"), ("yuyan", "语言"),
    ("yuyin", "语音"), ("zaijian", "再见"), ("zaixian", "在线"), ("zaoshang", "早上"),
    ("zenme", "怎么"), ("zenmeyang", "怎么样"), ("zhangdie", "涨跌"), ("zhangfu", "涨幅"),
    ("zhanghao", "账号"), ("zhejiang", "浙江"), ("zhekou", "折扣"), ("zheli", "这里"),
    ("zhengchang", "正常"), ("zhengque", "正确"), ("zhengzai", "正在"), ("zhiChi", "支持"),
    ("zhichi", "支持"), ("zhidao", "知道"), ("zhongguo", "中国"), ("zhongwen", "中文"),
    ("zhongyao", "重要"), ("zhuanfa", "转发"), ("zhuli", "助理"), ("zhuye", "主页"),
    ("zhuyi", "注意"), ("zixuan", "自选"), ("zixin", "自信"), ("zuihao", "最好"),
    ("zuijin", "最近"), ("zuotian", "昨天"), ("zuowei", "作为"),
    # more conversational
    ("baozheng", "保证"), ("biaoqing", "表情"), ("buke", "不可"), ("chihu", "吃苦"),
    ("chongfu", "重复"), ("chuanqi", "传奇"), ("cishu", "次数"), ("cuicu", "催促"),
    ("dabian", "答辩"), ("daguimo", "大规模"), ("daihao", "代号"), ("daikuan", "贷款"),
    ("danxin", "担心"), ("daoqian", "道歉"), ("denghou", "等候"), ("dianji", "点击"),
    ("dongxi", "东西"), ("duanxin", "短信"), ("duibi", "对比"), ("duihua", "对话"),
    ("duoyuan", "多元"), ("fabu", "发布"), ("facai", "发财"), ("fangjia", "放假"),
    ("fangjian", "房间"), ("fangfa", "方法"), ("fangwen", "访问"), ("fangxin", "放心"),
    ("feichang", "非常"), ("fenbian", "分辨"), ("fengxian", "风险"), ("fuqian", "付钱"),
    ("fuzhuang", "服装"), ("gaijin", "改进"), ("ganjue", "感觉"), ("gaibian", "改变"),
    ("gaodu", "高度"), ("gaoxiao", "高效"), ("geiwo", "给我"), ("genju", "根据"),
    ("gongneng", "功能"), ("gongkai", "公开"), ("gongping", "公平"), ("gongxi", "恭喜"),
    ("guanli", "管理"), ("guanyu", "关于"), ("guihai", "规划"), ("guihua", "规划"),
    ("guoji", "国际"), ("guoqing", "国情"), ("haokan", "好看"), ("haowan", "好玩"),
    ("haoxiang", "好像"), ("hezuo", "合作"), ("hongse", "红色"), ("houlai", "后来"),
    ("huanye", "换页"), ("huidao", "回到"), ("huitou", "回头"), ("hunyin", "婚姻"),
    ("huzhao", "护照"), ("jiahua", "假话"), ("jiagebiao", "价格表"), ("jiandanhao", "简单好"),
    ("jianghua", "讲话"), ("jiaotong", "交通"), ("jiaoyu", "教育"), ("jiben", "基本"),
    ("jiehuan", "借还"), ("jieshao", "介绍"), ("jiesheng", "节省"), ("jiezhe", "接着"),
    ("jinbu", "进步"), ("jingcai", "精彩"), ("jingli", "经理"), ("jingzheng", "竞争"),
    ("jinqian", "金钱"), ("jinzhang", "紧张"), ("jiuwei", "就位"), ("juece", "决策"),
    ("juede", "觉得"), ("kafeiui", "咖啡"), ("kaifang", "开放"), ("kaiguan", "开关"),
    ("kanfa", "看法"), ("kaobei", "拷贝"), ("keai", "可爱"), ("kefu", "客服"),
    ("keguan", "客观"), ("kehu", "客户"), ("kongzhi", "控制"), ("kuaidi", "快递"),
    ("kuaile", "快乐"), ("laiwang", "来往"), ("laodong", "劳动"), ("laixin", "来信"),
    ("lianai", "恋爱"), ("lianghao", "良好"), ("lingdao", "领导"), ("linghuo", "灵活"),
    ("liwu", "礼物"), ("lvyou", "旅游"), ("mafan", "麻烦"), ("maoyi", "贸易"),
    ("meili", "美丽"), ("mianfei", "免费"), ("mianji", "面积"), ("minzu", "民族"),
    ("moshi", "模式"), ("mudi", "目的"), ("nengli", "能力"), ("nianling", "年龄"),
    ("nuli", "努力"), ("paiqi", "排气"), ("paizhao", "拍照"), ("peihe", "配合"),
    ("pianyi", "便宜"), ("piaozi", "票子"), ("pingheng", "平衡"), ("pingtai", "平台"),
    ("qianjin", "前进"), ("qianqi", "前期"), ("qiandai", "钱袋"), ("qingchuhao", "清楚"),
    ("qingjia", "请假"), ("qingnian", "青年"), ("qingzhu", "庆祝"), ("qubie", "区别"),
    ("qudao", "渠道"), ("quanbu", "全部"), ("queding", "确定"), ("renzhen", "认真"),
    ("rili", "日历"), ("rongliang", "容量"), ("rongyu", "荣誉"), ("saoma", "扫码"),
    ("shangpin", "商品"), ("shangsheng", "上升"), ("shangxin", "伤心"), ("shehui", "社会"),
    ("shengchan", "生产"), ("shenghuo", "生活"), ("shengri", "生日"), ("shenru", "深入"),
    ("shenpi", "审批"), ("shibie", "识别"), ("shifu", "师傅"), ("shijie", "世界"),
    ("shipinhao", "食品"), ("shouming", "寿命"), ("shouqi", "收起"), ("shouyi", "收益"),
    ("shouhuo", "收获"), ("shuiping", "水平"), ("shuohua", "说话"), ("shuxue", "数学"),
    ("sichuan", "四川"), ("siji", "司机"), ("suoyou", "所有"), ("taidu", "态度"),
    ("tisheng", "提升"), ("tishi", "提示"), ("tongguo", "通过"), ("tongyi", "同意"),
    ("touxiang", "头像"), ("tuandui", "团队"), ("waiyu", "外语"), ("wangzhan", "网站"),
    ("weidu", "维度"), ("wenhua", "文化"), ("wenjuan", "问卷"), ("wending", "稳定"),
    ("wuliao", "无聊"), ("wulianwang", "物联网"), ("xiansuo", "线索"), ("xiangfa", "想法"),
    ("xiangjiao", "香蕉"), ("xiangnian", "想念"), ("xiaoguo", "效果"), ("xiaoxue", "小学"),
    ("xiehui", "协会"), ("xieru", "写入"), ("xinlai", "信赖"), ("xinchun", "新春"),
    ("xingbie", "性别"), ("xingcheng", "行程"), ("xinghao", "型号"), ("xiugai", "修改"),
    ("xuanze", "选择"), ("xuexiao", "学校"), ("yanjiu", "研究"), ("yansehao", "颜色好"),
    ("yaoqiu", "要求"), ("yewu", "业务"), ("yiliao", "医疗"), ("yinsi", "隐私"),
    ("yingxiang", "影响"), ("yingxiao", "营销"), ("yiyi", "意义"), ("yongqi", "勇气"),
    ("youhao", "友好"), ("youhuiquan", "优惠券"), ("youqing", "友情"), ("yuanfang", "远方"),
    ("yuanlai", "原来"), ("yuedu", "阅读"), ("yunxu", "允许"), ("zaihu", "在乎"),
    ("zaocan", "早餐"), ("zengjia", "增加"), ("zhanghu", "账户"), ("zhaopian", "照片"),
    ("zhaung", "装"), ("zhengce", "政策"), ("zhengming", "证明"), ("zhiye", "职业"),
    ("zhongdian", "重点"), ("zhongxin", "中心"), ("zhuanye", "专业"), ("zhunbei", "准备"),
    ("ziyuan", "资源"), ("zongjie", "总结"), ("zuiba", "嘴巴"), ("zuoye", "作业"),
]

# Deduplicate by pinyin key (keep first).
_seen = set()
WORDS_DEDUP: list[tuple[str, str]] = []
for py, hz in WORDS:
    py = py.lower().strip()
    if not py or py in _seen:
        continue
    if not all("a" <= c <= "z" for c in py):
        continue
    _seen.add(py)
    WORDS_DEDUP.append((py, hz))
WORDS_DEDUP.sort(key=lambda x: x[0])

# Standard Mandarin syllables (toneless) with common characters — compact but usable.
SYL: list[tuple[str, str]] = [
    ("a", "啊阿呀"), ("ai", "爱矮挨癌艾碍"), ("an", "安按暗案岸"), ("ang", "昂"),
    ("ao", "奥傲熬澳袄"), ("ba", "吧把八爸巴罢拔"), ("bai", "白百摆败拜"),
    ("ban", "半办般版班板伴"), ("bang", "帮绑旁棒榜"), ("bao", "包报保饱宝抱暴"),
    ("bei", "被北备背杯倍悲"), ("ben", "本奔笨"), ("bi", "比必笔闭避鼻彼"),
    ("bian", "边变便遍编辩"), ("biao", "表标彪"), ("bie", "别憋"), ("bin", "宾滨"),
    ("bing", "并病兵冰饼"), ("bo", "波播博伯薄拨"), ("bu", "不部步布补捕"),
    ("ca", "擦"), ("cai", "才菜彩财采猜"), ("can", "参残餐惨"), ("cang", "藏仓苍"),
    ("cao", "草操曹"), ("ce", "测策侧册"), ("ceng", "曾层"), ("cha", "查差茶插察"),
    ("chai", "拆柴"), ("chan", "产缠禅"), ("chang", "长常场厂唱畅"), ("chao", "超朝潮吵抄"),
    ("che", "车彻撤"), ("chen", "陈沉晨尘称"), ("cheng", "成城程称承乘"),
    ("chi", "吃持池迟赤尺"), ("chong", "冲重充虫崇"), ("chou", "抽愁丑仇"),
    ("chu", "出处初除楚触"), ("chuan", "传穿船川串"), ("chuang", "创窗床闯"),
    ("chui", "吹垂"), ("chun", "春纯唇"), ("ci", "次此词刺磁"), ("cong", "从聪丛"),
    ("cou", "凑"), ("cu", "粗促"), ("cuan", "窜"), ("cui", "催脆翠"), ("cun", "存村寸"),
    ("cuo", "错措"), ("da", "大打达答搭"), ("dai", "带代待袋戴呆"),
    ("dan", "但单担弹蛋胆淡"), ("dang", "当党挡荡"), ("dao", "到道倒导刀岛"),
    ("de", "的得德地"), ("dei", "得"), ("deng", "等登灯邓"), ("di", "地第低底滴弟敌"),
    ("dian", "点电店典殿垫"), ("diao", "调掉吊雕"), ("die", "跌叠爹"), ("ding", "定顶订丁盯"),
    ("diu", "丢"), ("dong", "东动懂冬洞冻"), ("dou", "都斗豆逗"), ("du", "都读度独毒杜"),
    ("duan", "段断短端"), ("dui", "对队堆"), ("dun", "顿盾吨"), ("duo", "多朵夺躲"),
    ("e", "额恶饿俄"), ("en", "恩"), ("er", "而二儿耳尔"), ("fa", "发法罚乏"),
    ("fan", "反饭烦翻凡范"), ("fang", "方放房防访仿"), ("fei", "非飞费肥废"),
    ("fen", "分份纷粉奋"), ("feng", "风封丰峰疯缝"), ("fo", "佛"), ("fou", "否"),
    ("fu", "服复父府付富负妇"), ("ga", "嘎"), ("gai", "该改概盖"), ("gan", "干感敢赶甘杆"),
    ("gang", "刚钢港岗纲"), ("gao", "高告搞稿膏"), ("ge", "个各歌格哥隔"), ("gei", "给"),
    ("gen", "跟根"), ("geng", "更耕"), ("gong", "工公功共供宫攻"), ("gou", "够狗购沟构"),
    ("gu", "古故顾股固估"), ("gua", "挂瓜刮"), ("guai", "怪乖拐"), ("guan", "关管观官馆惯"),
    ("guang", "光广逛"), ("gui", "贵归鬼规柜"), ("gun", "滚棍"), ("guo", "过国果锅郭"),
    ("ha", "哈"), ("hai", "还海害孩咳"), ("han", "汉寒含喊汗韩"), ("hang", "行航杭"),
    ("hao", "好号毫豪耗"), ("he", "和合何河喝核盒"), ("hei", "黑嘿"), ("hen", "很恨痕"),
    ("heng", "横恒哼"), ("hong", "红洪宏轰"), ("hou", "后候厚侯"), ("hu", "护户湖互胡忽"),
    ("hua", "话花化画华滑"), ("huai", "坏怀"), ("huan", "还换欢环缓患"), ("huang", "黄皇慌晃"),
    ("hui", "会回灰辉汇毁"), ("hun", "混婚昏魂"), ("huo", "或活火货获"),
    ("ji", "机记几己计及集级急"), ("jia", "家加价假架甲"), ("jian", "见间件建简减健检"),
    ("jiang", "将讲江降奖"), ("jiao", "叫教交较脚角觉"), ("jie", "接解结界姐借街"),
    ("jin", "进金近今尽紧仅"), ("jing", "经京精静景警净"), ("jiong", "窘"),
    ("jiu", "就九久酒旧救"), ("ju", "就据句局举具居"), ("juan", "卷圈倦"), ("jue", "觉决绝脚"),
    ("jun", "军均君俊"), ("ka", "卡咖"), ("kai", "开凯楷"), ("kan", "看砍刊堪"),
    ("kang", "康抗扛"), ("kao", "考靠烤"), ("ke", "可课科克客刻"), ("ken", "肯恳"),
    ("keng", "坑"), ("kong", "空恐控孔"), ("kou", "口扣"), ("ku", "苦库哭酷"),
    ("kua", "跨夸"), ("kuai", "快块会"), ("kuan", "宽款"), ("kuang", "况狂框矿"),
    ("kui", "亏愧魁"), ("kun", "困昆"), ("kuo", "扩阔"), ("la", "啦拉辣蜡"),
    ("lai", "来赖莱"), ("lan", "蓝懒栏烂览"), ("lang", "浪郎朗狼"), ("lao", "老劳牢捞"),
    ("le", "了乐勒"), ("lei", "类累雷泪"), ("leng", "冷楞"), ("li", "里理力立利例离李"),
    ("lia", "俩"), ("lian", "连联脸练恋怜"), ("liang", "两亮量良凉粮"), ("liao", "了料聊疗"),
    ("lie", "列烈裂"), ("lin", "林临邻淋"), ("ling", "另令零领灵"), ("liu", "六流留刘柳"),
    ("long", "龙弄隆笼"), ("lou", "楼露漏搂"), ("lu", "路录陆鲁炉绿"), ("luan", "乱卵"),
    ("lue", "略"), ("lun", "论轮伦"), ("luo", "落罗洛络"), ("lv", "绿旅律率虑铝"),
    ("ma", "吗妈马骂麻码"), ("mai", "买卖麦迈"), ("man", "满慢漫蛮"), ("mang", "忙盲芒"),
    ("mao", "毛冒帽猫貌"), ("me", "么"), ("mei", "没每美梅妹媒"), ("men", "们门闷"),
    ("meng", "梦猛蒙盟"), ("mi", "米密迷秘蜜"), ("mian", "面免棉眠"), ("miao", "秒妙描苗"),
    ("mie", "灭蔑"), ("min", "民敏"), ("ming", "明名命鸣"), ("miu", "谬"),
    ("mo", "么模莫末摸墨"), ("mou", "某谋"), ("mu", "目母木亩幕慕"), ("na", "那拿哪纳娜"),
    ("nai", "乃奶耐"), ("nan", "难南男"), ("nang", "囊"), ("nao", "脑闹恼"), ("ne", "呢"),
    ("nei", "内那"), ("nen", "嫩"), ("neng", "能"), ("ni", "你尼泥逆拟"),
    ("nian", "年念粘"), ("niang", "娘酿"), ("niao", "鸟尿"), ("nie", "捏聂"), ("nin", "您"),
    ("ning", "宁凝"), ("niu", "牛纽扭"), ("nong", "农弄浓"), ("nu", "女努怒"),
    ("nuan", "暖"), ("nue", "虐"), ("nuo", "诺挪"), ("nv", "女"), ("o", "哦喔噢"),
    ("ou", "欧偶呕"), ("pa", "怕爬帕"), ("pai", "派排拍牌"), ("pan", "盘判盼叛"),
    ("pang", "旁胖庞"), ("pao", "跑炮泡抛"), ("pei", "配陪培佩"), ("pen", "喷盆"),
    ("peng", "朋碰鹏捧"), ("pi", "批皮疲匹辟"), ("pian", "片偏骗篇"), ("piao", "票飘漂"),
    ("pie", "撇"), ("pin", "品贫拼频"), ("ping", "平评凭瓶屏"), ("po", "破坡颇婆迫"),
    ("pou", "剖"), ("pu", "普铺扑仆"), ("qi", "起其气期七器奇齐"), ("qia", "恰卡"),
    ("qian", "前钱千签浅欠"), ("qiang", "强墙抢枪"), ("qiao", "桥巧悄敲乔"),
    ("qie", "且切窃"), ("qin", "亲侵勤琴"), ("qing", "请情清青轻庆"), ("qiong", "穷琼"),
    ("qiu", "求球秋仇"), ("qu", "去取区曲趣"), ("quan", "全权泉圈劝"), ("que", "却确缺雀"),
    ("qun", "群裙"), ("ran", "然染燃"), ("rang", "让嚷"), ("rao", "绕扰"), ("re", "热惹"),
    ("ren", "人认任忍仁"), ("reng", "仍扔"), ("ri", "日"), ("rong", "容荣融溶"),
    ("rou", "肉柔"), ("ru", "如入乳儒"), ("ruan", "软"), ("rui", "瑞锐"), ("run", "润"),
    ("ruo", "若弱"), ("sa", "萨撒洒"), ("sai", "赛塞"), ("san", "三散伞"), ("sang", "桑丧"),
    ("sao", "扫骚"), ("se", "色涩"), ("sen", "森"), ("seng", "僧"), ("sha", "啥沙杀傻纱"),
    ("shai", "晒筛"), ("shan", "山善闪删衫"), ("shang", "上商伤尚赏"), ("shao", "少绍烧稍勺"),
    ("she", "社设舍射涉"), ("shei", "谁"), ("shen", "什身深神甚伸"), ("sheng", "生声省升胜圣"),
    ("shi", "是时十事使市实识"), ("shou", "手受收首守寿"), ("shu", "书数树属输熟术"),
    ("shua", "刷耍"), ("shuai", "帅摔衰"), ("shuan", "栓"), ("shuang", "双爽"),
    ("shui", "谁水睡税"), ("shun", "顺瞬"), ("shuo", "说硕"), ("si", "四死思司似私"),
    ("song", "送松宋颂"), ("sou", "搜艘"), ("su", "速诉素苏宿"), ("suan", "算酸蒜"),
    ("sui", "随岁碎虽"), ("sun", "孙损"), ("suo", "所锁缩索"), ("ta", "他她它塔踏"),
    ("tai", "太台态抬泰"), ("tan", "谈弹探叹贪"), ("tang", "唐堂汤糖躺"), ("tao", "套逃桃讨淘"),
    ("te", "特"), ("tei", "忒"), ("teng", "疼腾"), ("ti", "体提题替梯踢"),
    ("tian", "天田甜填添"), ("tiao", "条调跳挑"), ("tie", "贴铁"), ("ting", "听停庭挺"),
    ("tong", "同通统痛童铜"), ("tou", "头投透偷"), ("tu", "图土突途徒"), ("tuan", "团"),
    ("tui", "退推腿"), ("tun", "吞屯"), ("tuo", "托脱妥拖"), ("wa", "瓦挖哇蛙"),
    ("wai", "外歪"), ("wan", "完万晚碗玩弯"), ("wang", "往网王忘望亡"),
    ("wei", "为位未卫微围伟"), ("wen", "问文闻温稳"), ("weng", "翁嗡"), ("wo", "我握卧沃"),
    ("wu", "无五物务武误屋"), ("xi", "西系喜息希细习"), ("xia", "下夏吓侠狭"),
    ("xian", "先现线显限县"), ("xiang", "想向相香像响项"), ("xiao", "小笑校消效晓"),
    ("xie", "些写谢协鞋斜"), ("xin", "新心信欣辛"), ("xing", "行性星兴形幸"),
    ("xiong", "兄雄胸凶"), ("xiu", "修休秀袖"), ("xu", "需要许续虚绪"), ("xuan", "选宣旋悬"),
    ("xue", "学血雪穴"), ("xun", "寻训迅询循"), ("ya", "呀压牙亚雅鸭"),
    ("yan", "眼言研严演验烟"), ("yang", "样阳杨养洋扬"), ("yao", "要药摇腰咬遥"),
    ("ye", "也业夜叶页野"), ("yi", "一以已意义易衣依"), ("yin", "因音银引印阴"),
    ("ying", "应影英营迎硬"), ("yo", "哟"), ("yong", "用永勇拥泳庸"),
    ("you", "有又由友好右油"), ("yu", "与于语雨余预鱼"), ("yuan", "原远元院员愿圆"),
    ("yue", "月越约乐阅"), ("yun", "云运员允晕"), ("za", "杂砸"), ("zai", "在再载灾"),
    ("zan", "咱赞暂"), ("zang", "脏藏"), ("zao", "早造遭糟澡"), ("ze", "则责择泽"),
    ("zei", "贼"), ("zen", "怎"), ("zeng", "增曾赠"), ("zha", "炸扎眨诈"),
    ("zhai", "摘窄债"), ("zhan", "站战展占沾"), ("zhang", "长张掌涨章帐"),
    ("zhao", "找照招着赵"), ("zhe", "这着者折哲"), ("zhei", "这"), ("zhen", "真针阵震镇"),
    ("zheng", "正整政争证征"), ("zhi", "只知之直纸至制指"), ("zhong", "中种重众终钟"),
    ("zhou", "周州洲舟昼"), ("zhu", "主住注助猪竹逐"), ("zhua", "抓爪"), ("zhuai", "拽"),
    ("zhuan", "转专传砖"), ("zhuang", "装状壮撞庄"), ("zhui", "追坠"), ("zhun", "准"),
    ("zhuo", "桌着捉浊"), ("zi", "子自字资紫"), ("zong", "总宗综纵"), ("zou", "走奏"),
    ("zu", "组足族祖阻"), ("zuan", "钻"), ("zui", "最嘴罪醉"), ("zun", "尊遵"),
    ("zuo", "做作坐左昨座"),
]
SYL.sort(key=lambda x: x[0])

HEADER = '''#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One IME candidate: UTF-8 text + how many input units to consume
 * (letters for 26-key / abc pinyin, digits for T9). */
typedef struct {
    const char *text;
    uint8_t consume;
} muse_py_hit_t;

/* Toneless pinyin letter buffer (a-z), e.g. "nihao" / "taiwan". */
int muse_pinyin_lookup(const char *py, muse_py_hit_t *out, int max);

/* T9 digit buffer (2-9), e.g. "644266" for nihao, "824926" for taiwan. */
int muse_pinyin_lookup_t9(const char *digits, muse_py_hit_t *out, int max);

/* Map a-z to T9 digit '2'..'9'; 0 if not a letter. */
char muse_pinyin_letter_digit(char c);

#ifdef __cplusplus
}
#endif
'''

ENGINE = r'''
#include "muse_pinyin.h"
#include <string.h>

typedef struct { const char *py; const char *cands; } muse_py_entry_t;

'''

def c_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')

def emit_table(name: str, rows: list[tuple[str, str]]) -> str:
    lines = [f"static const muse_py_entry_t {name}[] = {{"]
    for py, cands in rows:
        lines.append(f'    {{ "{c_escape(py)}", "{c_escape(cands)}" }},')
    lines.append("};")
    lines.append(f"static const int {name}_n = {len(rows)};")
    return "\n".join(lines) + "\n"

LOOKUP = r'''
static char letter_digit(char c)
{
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c >= 'a' && c <= 'c') return '2';
    if (c >= 'd' && c <= 'f') return '3';
    if (c >= 'g' && c <= 'i') return '4';
    if (c >= 'j' && c <= 'l') return '5';
    if (c >= 'm' && c <= 'o') return '6';
    if (c >= 'p' && c <= 's') return '7';
    if (c >= 't' && c <= 'v') return '8';
    if (c >= 'w' && c <= 'z') return '9';
    return 0;
}

char muse_pinyin_letter_digit(char c) { return letter_digit(c); }

static int encode_digits(const char *py, char *out, int max)
{
    int n = 0;
    for (; *py && n < max - 1; py++) {
        char d = letter_digit(*py);
        if (!d) return -1;
        out[n++] = d;
    }
    out[n] = 0;
    return n;
}

static int utf8_char_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

#define SLOT_MAX 64
#define SLOT_BYTES 16
static char s_slots[SLOT_MAX][SLOT_BYTES];
static int s_slot_i;

static const char *slot_copy(const char *s, int nbytes)
{
    if (nbytes <= 0 || nbytes >= SLOT_BYTES) return NULL;
    if (s_slot_i >= SLOT_MAX) s_slot_i = 0;
    char *d = s_slots[s_slot_i++];
    memcpy(d, s, (size_t)nbytes);
    d[nbytes] = 0;
    return d;
}

static int already(const muse_py_hit_t *out, int n, const char *s)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(out[i].text, s) == 0) return 1;
    }
    return 0;
}

static int push_hit(muse_py_hit_t *out, int n, int max, const char *text, int consume)
{
    if (n >= max || !text || !text[0] || consume <= 0) return n;
    if (already(out, n, text)) return n;
    out[n].text = text;
    out[n].consume = (uint8_t)(consume > 255 ? 255 : consume);
    return n + 1;
}

static int push_chars(muse_py_hit_t *out, int n, int max, const char *cands, int consume, int limit)
{
    int got = 0;
    const char *c = cands;
    while (*c && n < max && got < limit) {
        int cl = utf8_char_len((unsigned char)*c);
        const char *s = slot_copy(c, cl);
        if (s) n = push_hit(out, n, max, s, consume);
        c += cl;
        got++;
    }
    return n;
}

/* Longest syllable that is a prefix of `py` (letters). */
static int longest_syl_prefix(const char *py, const char **syl_out)
{
    int best = 0;
    const char *best_s = NULL;
    size_t plen = strlen(py);
    for (int i = 0; i < s_syl_n; i++) {
        size_t klen = strlen(s_syl[i].py);
        if ((int)klen > best && klen <= plen && strncmp(py, s_syl[i].py, klen) == 0) {
            best = (int)klen;
            best_s = s_syl[i].cands;
        }
    }
    if (syl_out) *syl_out = best_s;
    return best;
}

/* Longest syllable whose digit encoding is a prefix of `digits`. */
static int longest_syl_digit_prefix(const char *digits, const char **syl_out, int *syl_dlen)
{
    int best = 0;
    const char *best_s = NULL;
    size_t dlen = strlen(digits);
    char buf[16];
    for (int i = 0; i < s_syl_n; i++) {
        int n = encode_digits(s_syl[i].py, buf, (int)sizeof(buf));
        if (n <= 0) continue;
        if (n > best && (size_t)n <= dlen && strncmp(digits, buf, (size_t)n) == 0) {
            best = n;
            best_s = s_syl[i].cands;
        }
    }
    if (syl_out) *syl_out = best_s;
    if (syl_dlen) *syl_dlen = best;
    return best;
}

static int lookup_letters(const char *py, muse_py_hit_t *out, int max)
{
    int n = 0;
    s_slot_i = 0;
    size_t plen = strlen(py);
    if (!plen) return 0;

    /* 1) Exact word */
    for (int i = 0; i < s_words_n && n < max; i++) {
        if (strcmp(s_words[i].py, py) == 0) {
            n = push_hit(out, n, max, s_words[i].cands, (int)plen);
        }
    }
    /* 2) Word where typed is a prefix (niha -> 你好) */
    for (int i = 0; i < s_words_n && n < max; i++) {
        size_t klen = strlen(s_words[i].py);
        if (klen > plen && strncmp(s_words[i].py, py, plen) == 0) {
            n = push_hit(out, n, max, s_words[i].cands, (int)klen);
        }
    }
    /* 3) Exact syllable -> chars */
    for (int i = 0; i < s_syl_n && n < max; i++) {
        if (strcmp(s_syl[i].py, py) == 0) {
            n = push_chars(out, n, max, s_syl[i].cands, (int)plen, 8);
        }
    }
    /* 4) Partial syllable (ni -> 你… when still typing nin/ning/…) */
    if (n < max) {
        for (int i = 0; i < s_syl_n && n < max; i++) {
            size_t klen = strlen(s_syl[i].py);
            if (klen > plen && strncmp(s_syl[i].py, py, plen) == 0) {
                n = push_chars(out, n, max, s_syl[i].cands, (int)klen, 3);
            }
        }
    }
    /* 5) Multi-syllable in progress: offer first complete syllable chars
     *    (nihao -> 你 with consume=2, leaving hao). */
    {
        const char *sc = NULL;
        int sl = longest_syl_prefix(py, &sc);
        if (sl > 0 && (size_t)sl < plen && sc) {
            n = push_chars(out, n, max, sc, sl, 6);
        }
    }
    return n;
}

static int lookup_t9(const char *digits, muse_py_hit_t *out, int max)
{
    int n = 0;
    s_slot_i = 0;
    size_t dlen = strlen(digits);
    if (!dlen) return 0;
    char dbuf[32];

    /* Validate digits */
    for (size_t i = 0; i < dlen; i++) {
        if (digits[i] < '2' || digits[i] > '9') return 0;
    }

    /* 1) Words whose digit encoding equals typed */
    for (int i = 0; i < s_words_n && n < max; i++) {
        int dn = encode_digits(s_words[i].py, dbuf, (int)sizeof(dbuf));
        if (dn == (int)dlen && strcmp(dbuf, digits) == 0) {
            n = push_hit(out, n, max, s_words[i].cands, (int)dlen);
        }
    }
    /* 2) Words where typed digits are a prefix of word digits */
    for (int i = 0; i < s_words_n && n < max; i++) {
        int dn = encode_digits(s_words[i].py, dbuf, (int)sizeof(dbuf));
        if (dn > (int)dlen && strncmp(dbuf, digits, dlen) == 0) {
            n = push_hit(out, n, max, s_words[i].cands, dn);
        }
    }
    /* 3) Exact syllable digit match -> chars */
    for (int i = 0; i < s_syl_n && n < max; i++) {
        int dn = encode_digits(s_syl[i].py, dbuf, (int)sizeof(dbuf));
        if (dn == (int)dlen && strcmp(dbuf, digits) == 0) {
            n = push_chars(out, n, max, s_syl[i].cands, (int)dlen, 8);
        }
    }
    /* 4) Partial syllable (digit prefix) */
    if (n < max) {
        for (int i = 0; i < s_syl_n && n < max; i++) {
            int dn = encode_digits(s_syl[i].py, dbuf, (int)sizeof(dbuf));
            if (dn > (int)dlen && strncmp(dbuf, digits, dlen) == 0) {
                n = push_chars(out, n, max, s_syl[i].cands, dn, 3);
            }
        }
    }
    /* 5) First complete syllable under digit buffer */
    {
        const char *sc = NULL;
        int sdl = 0;
        longest_syl_digit_prefix(digits, &sc, &sdl);
        if (sdl > 0 && (size_t)sdl < dlen && sc) {
            n = push_chars(out, n, max, sc, sdl, 6);
        }
    }
    return n;
}

int muse_pinyin_lookup(const char *py, muse_py_hit_t *out, int max)
{
    if (!py || !out || max <= 0) return 0;
    return lookup_letters(py, out, max);
}

int muse_pinyin_lookup_t9(const char *digits, muse_py_hit_t *out, int max)
{
    if (!digits || !out || max <= 0) return 0;
    return lookup_t9(digits, out, max);
}
'''

def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(TEST_DIR, exist_ok=True)
    c_path = os.path.join(OUT_DIR, "muse_pinyin.c")
    h_path = os.path.join(OUT_DIR, "muse_pinyin.h")
    with open(h_path, "w", encoding="utf-8") as f:
        f.write(HEADER)
    body = ENGINE + emit_table("s_words", WORDS_DEDUP) + "\n" + emit_table("s_syl", SYL) + "\n" + LOOKUP
    with open(c_path, "w", encoding="utf-8") as f:
        f.write(body)
    print(f"Wrote {h_path}")
    print(f"Wrote {c_path} words={len(WORDS_DEDUP)} syl={len(SYL)}")

    # Host unit test (no LVGL)
    test_c = os.path.join(TEST_DIR, "test_muse_pinyin.c")
    with open(test_c, "w", encoding="utf-8") as f:
        f.write(textwrap.dedent(r'''
        #include "muse_pinyin.h"
        #include <stdio.h>
        #include <string.h>
        #include <stdlib.h>

        static int g_fail;

        static int has_text(const muse_py_hit_t *h, int n, const char *t)
        {
            for (int i = 0; i < n; i++) if (strcmp(h[i].text, t) == 0) return 1;
            return 0;
        }

        static void expect_has(const char *tag, const muse_py_hit_t *h, int n, const char *t)
        {
            if (!has_text(h, n, t)) {
                fprintf(stderr, "FAIL %s: missing candidate '%s' (n=%d):", tag, t, n);
                for (int i = 0; i < n; i++) fprintf(stderr, " [%s/%u]", h[i].text, h[i].consume);
                fprintf(stderr, "\n");
                g_fail++;
            } else {
                printf("OK   %s has '%s'\n", tag, t);
            }
        }

        static void expect_n_gt(const char *tag, int n, int min)
        {
            if (n < min) {
                fprintf(stderr, "FAIL %s: n=%d < %d\n", tag, n, min);
                g_fail++;
            } else {
                printf("OK   %s n=%d\n", tag, n);
            }
        }

        int main(void)
        {
            muse_py_hit_t hits[16];
            int n;

            /* 26-key letter accumulation */
            n = muse_pinyin_lookup("t", hits, 16);
            expect_n_gt("letter t", n, 1);
            /* should NOT be stuck on only taiwan — include ta-prefix words AND syllables */
            n = muse_pinyin_lookup("tai", hits, 16);
            expect_has("tai", hits, n, "台湾");
            expect_has("tai", hits, n, "太");

            n = muse_pinyin_lookup("taiwan", hits, 16);
            expect_has("taiwan", hits, n, "台湾");

            n = muse_pinyin_lookup("nihao", hits, 16);
            expect_has("nihao", hits, n, "你好");

            n = muse_pinyin_lookup("ninhao", hits, 16);
            expect_has("ninhao", hits, n, "您好");

            /* multi-key sequence: ni then hao — partial word */
            n = muse_pinyin_lookup("ni", hits, 16);
            expect_has("ni", hits, n, "你");
            expect_has("ni-word", hits, n, "你好");

            n = muse_pinyin_lookup("nih", hits, 16);
            expect_has("nih", hits, n, "你好");

            /* after first syllable when typing nihao, 你 available with consume=2 */
            n = muse_pinyin_lookup("nihao", hits, 16);
            {
                int found = 0;
                for (int i = 0; i < n; i++) {
                    if (strcmp(hits[i].text, "你") == 0 && hits[i].consume == 2) found = 1;
                }
                if (!found) {
                    /* 你好 exact may dominate; still require 你 with consume 2 from step 5 */
                    fprintf(stderr, "FAIL nihao: missing 你 consume=2\n");
                    for (int i = 0; i < n; i++)
                        fprintf(stderr, "  [%s/%u]\n", hits[i].text, hits[i].consume);
                    g_fail++;
                } else {
                    printf("OK   nihao has 你 consume=2\n");
                }
            }

            /* T9: taiwan = 8-2-4-9-2-6 */
            n = muse_pinyin_lookup_t9("8", hits, 16);
            expect_n_gt("t9 8", n, 1);
            n = muse_pinyin_lookup_t9("82", hits, 16);
            expect_n_gt("t9 82", n, 1);
            /* 82 should keep candidates (ta...), NOT clear */
            expect_has("t9 82 taiwan-prefix", hits, n, "台湾");

            n = muse_pinyin_lookup_t9("824926", hits, 16);
            expect_has("t9 taiwan", hits, n, "台湾");

            n = muse_pinyin_lookup_t9("644266", hits, 16);
            expect_has("t9 nihao", hits, n, "你好");

            n = muse_pinyin_lookup_t9("646426", hits, 16);
            expect_has("t9 ninhao", hits, n, "您好");

            /* T9 multi-key: 8 then 5 (TUV+JKL) — may have few/no words, but must refresh
             * without crashing; prefix-of-syllable path still OK to return 0. */
            n = muse_pinyin_lookup_t9("85", hits, 16);
            printf("OK   t9 85 n=%d (may be 0; must not stick on 台湾)\n", n);
            if (has_text(hits, n, "台湾")) {
                fprintf(stderr, "FAIL t9 85 should not still list 台湾\n");
                g_fail++;
            }

            /* letter digit helper */
            if (muse_pinyin_letter_digit('t') != '8' || muse_pinyin_letter_digit('a') != '2') {
                fprintf(stderr, "FAIL letter_digit\n");
                g_fail++;
            } else {
                printf("OK   letter_digit\n");
            }

            if (g_fail) {
                fprintf(stderr, "\n%d failure(s)\n", g_fail);
                return 1;
            }
            printf("\nAll pinyin IME tests passed.\n");
            return 0;
        }
        '''))
    print(f"Wrote {test_c}")

if __name__ == "__main__":
    main()
