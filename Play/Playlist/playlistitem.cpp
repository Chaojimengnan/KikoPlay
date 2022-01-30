#include "playlistitem.h"
#include "playlist.h"
#include "Play/Danmu/Manager/danmumanager.h"
#include "globalobjects.h"

PlayList* PlayListItem::playlist=nullptr;

PlayListItem::PlayListItem(PlayListItem *p, bool leaf, int insertPosition):
    parent(p),children(nullptr),playTime(0),playTimeState(UNPLAY),level(0),isBgmCollection(false), marker(M_NONE), addTime(0)
{
    if(!leaf)
    {
        children=new QList<PlayListItem *>();
    }
    if(parent)
    {
        if (insertPosition == -1)
            parent->children->append(this);
        else
            parent->children->insert(insertPosition, this);
        level=parent->level+1;
    }
}
PlayListItem::~PlayListItem()
{
    playlist->checkCurrentItem(this);
    if(children)
    {
        qDeleteAll(children->begin(),children->end());
        delete children;
    }
}

bool PlayListItem::hasPool() const
{
    return !poolID.isEmpty() && GlobalObjects::danmuManager->getPool(poolID, false);
}
void PlayListItem::setLevel(int newLevel)
{
    level=newLevel;
    if(children)
    {
        for(PlayListItem *child: qAsConst(*children))
        {
            child->setLevel(newLevel+1);
        }
    }
}
void PlayListItem::moveTo(PlayListItem *newParent, int insertPosition)
{
    if (parent) parent->children->removeAll(this);
    if (newParent)
    {
        if (insertPosition == -1)
            newParent->children->append(this);
        else
            newParent->children->insert(insertPosition, this);
        setLevel(newParent->level + 1);
    }
    parent = newParent;
}
