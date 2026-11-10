"""Build the Chinese player manual from verified Markdown and the live node/card catalogs.

Requirements: reportlab. Fonts default to Microsoft YaHei on Windows.
Run: python scripts/build_manual.py
"""
from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    BaseDocTemplate, Flowable, Frame, KeepTogether, PageBreak,
    PageTemplate, Paragraph, Spacer, Table, TableStyle, Image,
)
from reportlab.platypus.tableofcontents import TableOfContents

ROOT = Path(__file__).resolve().parents[1]
INK = colors.HexColor('#183137')
TEAL = colors.HexColor('#176C68')
MUTED = colors.HexColor('#59716F')
PAPER = colors.HexColor('#FCFAF5')
LINE = colors.HexColor('#D8E1DB')
GOLD = colors.HexColor('#DBAC66')
W, H = A4
LEFT, RIGHT, BOTTOM, TOP = 48, 48, 48, 58
WIDTH = W - LEFT - RIGHT


def safe(text: str) -> str:
    text = html.escape(text.strip())
    text = re.sub(r'`([^`]+)`', r'<font color="#176C68">\1</font>', text)
    return re.sub(r'\*\*([^*]+)\*\*', r'<b>\1</b>', text)


class Cover(Flowable):
    def __init__(self):
        Flowable.__init__(self)
        self.width, self.height = WIDTH, H - TOP - BOTTOM - 12

    def draw(self):
        c = self.canv
        c.saveState()
        c.setFillColor(INK)
        c.rect(-LEFT, -H, W, H * 2, fill=1, stroke=0)
        c.setStrokeColor(colors.HexColor('#29484B'))
        for x in range(-30, 600, 36):
            c.line(x, 80, x, 330)
        for y in range(80, 331, 36):
            c.line(-LEFT, y, W, y)
        c.setFillColor(GOLD)
        c.setFont('Body', 11)
        c.drawString(0, 663, 'CRINGE BLADE ZERO / PLAYER MANUAL')
        c.setFillColor(colors.white)
        c.setFont('Bold', 43)
        c.drawString(0, 583, '豪之刃')
        c.setFont('Bold', 24)
        c.drawString(0, 533, '功能与节点手册')
        c.setFillColor(colors.HexColor('#CBDDD4'))
        c.setFont('Body', 12)
        c.drawString(0, 485, '从安装、角色与战斗工具，到每一种派生节点。')
        c.drawString(0, 460, '完整规则 · 节点实图 · 界面图解 · 排错方法')
        labels = [('输入', 0, 235), ('招式', 175, 235), ('下一招', 350, 235), ('裁剪', 175, 130), ('加速', 350, 130)]
        c.setStrokeColor(GOLD)
        for x1,y1,x2,y2 in [(124,256,175,256),(299,256,350,256),(237,235,237,172),(299,245,350,151)]:
            c.line(x1,y1,x2,y2)
        for label, x, y in labels:
            c.setFillColor(colors.HexColor('#214348'))
            c.setStrokeColor(colors.HexColor('#668B80'))
            c.roundRect(x,y,124,42,7,fill=1,stroke=1)
            c.setFillColor(colors.white); c.setFont('Bold',13)
            c.drawCentredString(x+62,y+14,label)
        c.setFillColor(GOLD);c.setFont('Bold',16)
        c.drawString(0,58,'30 种节点 / 75 个外观 / sampleCombo')
        c.setFillColor(colors.HexColor('#CBDDD4'));c.setFont('Body',10)
        c.drawString(0,24,'2026.11.10  |  中文版  |  Steam / WeGame 共用 DLL')
        c.drawString(0,3,'本地与联机测试的边界，在对应章节明确说明。')
        c.restoreState()


class FlowDiagram(Flowable):
    def __init__(self, labels):
        Flowable.__init__(self)
        self.labels = labels
        self.width, self.height = WIDTH, 64

    def draw(self):
        c=self.canv;count=len(self.labels);gap=24;box=(WIDTH-gap*(count-1))/count
        for i,label in enumerate(self.labels):
            x=i*(box+gap)
            c.setFillColor(colors.HexColor('#E8F0E9'));c.setStrokeColor(LINE)
            c.roundRect(x,12,box,40,5,fill=1,stroke=1)
            p=Paragraph(safe(label), STYLES['diagram'])
            _,ph=p.wrap(box-12,36);p.drawOn(c,x+6,32-ph/2)
            if i<count-1:
                c.setStrokeColor(TEAL);c.setLineWidth(1.4);c.line(x+box+3,32,x+box+gap-4,32)
                c.line(x+box+gap-8,36,x+box+gap-4,32);c.line(x+box+gap-8,28,x+box+gap-4,32)


class ManualDoc(BaseDocTemplate):
    def __init__(self, filename):
        BaseDocTemplate.__init__(self,str(filename),pagesize=A4,leftMargin=LEFT,rightMargin=RIGHT,topMargin=TOP,bottomMargin=BOTTOM,
                                 title='豪之刃：完整功能与节点手册',author='Jersyi / Cringe Blade Zero',pageCompression=1)
        frame=Frame(LEFT,BOTTOM,WIDTH,H-TOP-BOTTOM,id='body',leftPadding=0,rightPadding=0,topPadding=0,bottomPadding=0)
        self.addPageTemplates(PageTemplate(id='manual',frames=[frame],onPage=self.decorate))
        self.section='使用指南';self.entries=[]

    def beforeDocument(self):
        self.section='使用指南';self.entries=[]

    def decorate(self,c,doc):
        if doc.page==1:return
        c.saveState();c.setFillColor(PAPER);c.rect(0,0,W,H,fill=1,stroke=0)
        c.setStrokeColor(LINE);c.setLineWidth(.6);c.line(LEFT,H-34,W-RIGHT,H-34);c.line(LEFT,34,W-RIGHT,34)
        c.setFillColor(MUTED);c.setFont('Body',8)
        c.drawString(LEFT,H-25,'豪之刃 / 功能与节点手册')
        c.drawRightString(W-RIGHT,H-25,'2026.11.10')
        c.drawString(LEFT,20,'CRINGE BLADE ZERO  ·  中文说明')
        c.setFont('Bold',9);c.drawRightString(W-RIGHT,20,f'{doc.page:02d}')
        c.restoreState()

    def afterFlowable(self,f):
        if isinstance(f,Paragraph) and hasattr(f,'section_key'):
            text=f.getPlainText();key=f.section_key
            self.canv.bookmarkPage(key);self.canv.addOutlineEntry(text,key,0,False)
            self.notify('TOCEntry',(0,text,self.page,key));self.entries.append({'title':text,'page':self.page})


STYLES={}


def initialize_fonts(font_dir):
    for name,filename in [('Body','msyh.ttc'),('Bold','msyhbd.ttc')]:
        pdfmetrics.registerFont(TTFont(name,str(font_dir/filename),subfontIndex=0))
    pdfmetrics.registerFontFamily('Body',normal='Body',bold='Bold',italic='Body',boldItalic='Bold')
    base=dict(fontName='Body',fontSize=10.4,leading=17,textColor=INK,wordWrap='CJK',alignment=TA_LEFT)
    STYLES['body']=ParagraphStyle('body',spaceAfter=9,**base)
    STYLES['h1']=ParagraphStyle('h1',fontName='Bold',fontSize=21,leading=30,textColor=INK,spaceAfter=18,wordWrap='CJK',keepWithNext=True)
    STYLES['h2']=ParagraphStyle('h2',fontName='Bold',fontSize=11.6,leading=18,textColor=TEAL,spaceBefore=10,spaceAfter=5,keepWithNext=True,wordWrap='CJK')
    STYLES['small']=ParagraphStyle('small',fontName='Body',fontSize=8.3,leading=13,textColor=MUTED,wordWrap='CJK')
    STYLES['cell']=ParagraphStyle('cell',fontName='Body',fontSize=9,leading=14,textColor=INK,wordWrap='CJK')
    STYLES['thead']=ParagraphStyle('thead',fontName='Bold',fontSize=9,leading=14,textColor=colors.white,wordWrap='CJK')
    STYLES['diagram']=ParagraphStyle('diagram',fontName='Bold',fontSize=10,leading=15,textColor=TEAL,wordWrap='CJK',alignment=1)


def table(rows,widths=None):
    cells=[[Paragraph(safe(cell),STYLES['thead'] if i==0 else STYLES['cell']) for cell in row] for i,row in enumerate(rows)]
    if widths is None:
        count=len(rows[0]);widths=[WIDTH/count]*count
        if count==2:widths=[WIDTH*.23,WIDTH*.77]
    t=Table(cells,colWidths=widths,repeatRows=1,hAlign='LEFT',splitByRow=1)
    t.setStyle(TableStyle([
        ('BACKGROUND',(0,0),(-1,0),TEAL),('ROWBACKGROUNDS',(0,1),(-1,-1),[colors.white,colors.HexColor('#EFF3ED')]),
        ('VALIGN',(0,0),(-1,-1),'TOP'),('LEFTPADDING',(0,0),(-1,-1),8),('RIGHTPADDING',(0,0),(-1,-1),8),
        ('TOPPADDING',(0,0),(-1,-1),7),('BOTTOMPADDING',(0,0),(-1,-1),7),('LINEBELOW',(0,-1),(-1,-1),.5,LINE),
    ]));return t


def parse_body(lines):
    out=[];pending=[];rows=[]
    def flush():
        if pending:out.append(Paragraph(safe(' '.join(pending)),STYLES['body']));pending.clear()
        if rows:out.extend([table(rows),Spacer(1,10)]);rows.clear()
    for line in lines:
        line=line.strip()
        if not line or line.startswith('<!--'):flush();continue
        picture=re.fullmatch(r'!\[([^\]]*)\]\((manual_images/[\w.]+)\)',line)
        if picture:
            flush();path=ROOT/'docs'/picture[2];pic=Image(str(path));is_ui=path.name.startswith('ui_')
            scale=min((WIDTH if is_ui else 330)/pic.imageWidth,(430 if is_ui else 155)/pic.imageHeight)
            pic.drawWidth=pic.imageWidth*scale;pic.drawHeight=pic.imageHeight*scale;pic.hAlign='LEFT'
            out.append(KeepTogether([pic,Spacer(1,5),Paragraph(safe(picture[1]),STYLES['small']),Spacer(1,10)]))
        elif line.startswith('### '):flush();out.append(Paragraph(safe(line[4:]),STYLES['h2']))
        elif line.startswith('@flow '):flush();out.append(FlowDiagram([x.strip() for x in line[6:].split('>')]))
        elif line.startswith('|'):
            if pending:out.append(Paragraph(safe(' '.join(pending)),STYLES['body']));pending.clear()
            cells=[x.strip() for x in line.strip('|').split('|')]
            if not all(re.fullmatch(r'[-: ]+',x) for x in cells):rows.append(cells)
        elif line.startswith('- '):flush();out.append(Paragraph('• '+safe(line[2:]),STYLES['body']))
        else:
            if rows:flush()
            pending.append(line)
    flush();return out


def supplement_cards():
    layout=(ROOT/'src/supplement/supplement_layout.h').read_text(encoding='utf-8-sig')
    generated=(ROOT/'src/generated/debug_supplement_playseq.generated.h').read_text(encoding='utf-8-sig')
    by_key={}
    for match in re.finditer(r'inline constexpr Card (\w+)\[\] = \{\n(.*?)\n};',layout+'\n'+generated,re.S):
        cards=[]
        for line in match[2].splitlines():
            values=re.findall(r'L"((?:[^"\\]|\\.)*)"',line)
            if len(values)<2:continue
            title,note,*options=values
            cards.append({'title':title,'note':note,'options':options})
        by_key[match[1]]=cards
    sections=[]
    for m in re.finditer(r'\{L"([^"]+)", L"([^"]*)", (k\w+), std::size\(',layout):
        sections.append({'name':m[1],'note':m[2],'cards':by_key[m[3]]})
    assert len(sections)==11
    assert sum(len(s['cards']) for s in sections)==91
    assert sum(len(c['options']) for s in sections for c in s['cards'])==482
    return sections


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,default=ROOT/'output/pdf/CringeBladeZero_Manual_ZH.pdf')
    parser.add_argument('--font-dir',type=Path,default=Path('C:/Windows/Fonts'))
    args=parser.parse_args();initialize_fonts(args.font_dir)
    source=ROOT/'docs/USER_MANUAL.md';text=source.read_text(encoding='utf-8-sig')
    model=(ROOT/'src/character/npc_combo_model.h').read_text(encoding='utf-8-sig')
    enum=re.search(r'enum class NodeType : int \{(.*?)\};',model,re.S)[1]
    types=[x.strip() for x in enum.split(',')]
    documented=re.findall(r'<!-- node: (\w+) -->',text)
    assert len(types)==30 and len(documented)==30 and set(types)==set(documented),'Node coverage mismatch'
    images=re.findall(r'!\[[^\]]*\]\((manual_images/[\w.]+)\)',text)
    assert all(f'manual_images/node_{name}.png' in images for name in types),'Node image coverage mismatch'
    assert all((ROOT/'docs'/path).is_file() for path in images),'Missing image'
    parts=re.split(r'^## (.+)$',text,flags=re.M)
    sections=[(parts[i],parts[i+1]) for i in range(1,len(parts),2)]
    story=[Cover(),PageBreak(),Paragraph('阅读导航',STYLES['h1']),Paragraph('点击章节标题或 PDF 书签跳转；每种节点独立成页，便于边编辑边查阅。',STYLES['body'])]
    toc=TableOfContents();toc.levelStyles=[ParagraphStyle('TOC',fontName='Body',fontSize=9.1,leading=13.3,textColor=INK,spaceBefore=0,wordWrap='CJK',firstLineIndent=0,leftIndent=0)]
    toc.dotsMinLevel=0;story.extend([toc,PageBreak()])
    for i,(title,body) in enumerate(sections):
        if i:story.append(PageBreak())
        h=Paragraph(safe(title),STYLES['h1']);h.section_key=f'section_{i}'
        story.extend([h,*parse_body(body.splitlines())])
    cards=supplement_cards();story.append(PageBreak())
    h=Paragraph('附录：调试补充卡片全索引',STYLES['h1']);h.section_key='supplement_index';story.append(h)
    story.append(Paragraph('自动核对当前目录：11 个分区、91 张卡片、482 个选项。下表列出全部卡片和用途；地图序列的具体选项以游戏内搜索列表为准。',STYLES['body']))
    for section in cards:
        story.append(Paragraph(safe(section['name']),STYLES['h2']))
        rows=[['卡片 / 功能','说明与可选操作']]
        for card in section['cards']:
            options=' / '.join(card['options']) if section['name']!='过场播放' else f"{len(card['options'])} 个序列子选项，支持按名称搜索。"
            note='；'.join(x for x in [card['note'],options] if x)
            rows.append([card['title'],note.replace('\\n',' ').replace('\\r',' ')])
        story.extend([table(rows,[WIDTH*.28,WIDTH*.72]),Spacer(1,12)])
    args.output.parent.mkdir(parents=True,exist_ok=True)
    doc=ManualDoc(args.output);doc.multiBuild(story)
    audit={'node_types':types,'documented_types':documented,'node_coverage':True,'sections':doc.entries,
           'supplement_sections':len(cards),'supplement_cards':91,'supplement_options':482,
           'images':images,'node_image_coverage':True,'ui_image_count':sum('/ui_' in name for name in images),
           'markdown_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
           'pdf_sha256':hashlib.sha256(args.output.read_bytes()).hexdigest(),'pages':doc.page}
    args.output.with_suffix('.coverage.json').write_text(json.dumps(audit,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'output':str(args.output),'pages':doc.page,'nodes':len(documented),'supplement_cards':91},ensure_ascii=False))


if __name__=='__main__':main()
