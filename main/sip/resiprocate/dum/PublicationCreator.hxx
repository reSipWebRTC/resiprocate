#if !defined(RESIP_PUBLICATIONCREATOR_HXX)
#define RESIP_PUBLICATIONCREATOR_HXX

#include "resiprocate/dum/BaseCreator.hxx"

namespace resip
{

class Uri;
class Contents;

class PublicationCreator: public BaseCreator
{
   public:
      PublicationCreator(DialogUsageManager& dum, 
                         const Uri& targetDocument, 
                         const NameAddr& from,
                         const Contents& body, 
                         const Data& eventType, 
                         unsigned expireSeconds );
};
 
}

#endif
