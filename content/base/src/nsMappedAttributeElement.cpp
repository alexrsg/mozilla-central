/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set tw=80 expandtab softtabstop=2 ts=2 sw=2: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsMappedAttributeElement.h"
#include "nsIDocument.h"

nsresult
nsMappedAttributeElement::WalkContentStyleRules(nsRuleWalker* aRuleWalker)
{
  mAttrsAndChildren.WalkMappedAttributeStyleRules(aRuleWalker);
  return NS_OK;
}

bool
nsMappedAttributeElement::SetMappedAttribute(nsIDocument* aDocument,
                                             nsIAtom* aName,
                                             nsAttrValue& aValue,
                                             nsresult* aRetval)
{
  NS_PRECONDITION(aDocument == GetCurrentDoc(), "Unexpected document");
  nsHTMLStyleSheet* sheet = aDocument ?
    aDocument->GetAttributeStyleSheet() : nullptr;

  *aRetval = mAttrsAndChildren.SetAndTakeMappedAttr(aName, aValue,
                                                    this, sheet);
  return true;
}

nsMapRuleToAttributesFunc
nsMappedAttributeElement::GetAttributeMappingFunction() const
{
  return &MapNoAttributesInto;
}

void
nsMappedAttributeElement::MapNoAttributesInto(const nsMappedAttributes* aAttributes,
                                              nsRuleData* aData)
{
}
